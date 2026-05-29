/*
 * Aspiradora.c  –  Cerebro principal de la Aspiradora Inteligente
 * ─────────────────────────────────────────────────────────────────
 * Hardware:  NXP KL25Z (ARM Cortex-M0+, 48 MHz)
 * Periféricos:
 *   UART0  PTA1/PTA2  115200 bps  →  Terminal debug (polling)
 *   UART1  PTE0/PTE1    9600 bps  →  ESP32-C3 BLE  (IRQ + ring buffer)
 *   I2C1   PTC10/PTC11            →  INA3221 (3 canales de energía)
 *   TPM0   PTA12/PTD4             →  HC-SR04 ultrasónico (state machine)
 *
 * Loop completamente no-bloqueante:
 *   - Ultrasónico: state machine + TPM0 CH4 input capture
 *   - I2C:  timeouts en todos los wait loops
 *   - AT:   ring buffer para RX, g_ms_ticks para timeouts
 *   - flag_1sec: consumido solo aquí (no en at_manager)
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "BSPInit.h"
#include "at_manager.h"
#include "isr.h"
#include "ultrasonico.h"
#include "INA3221.h"

/* ── Configuración ────────────────────────────────────────────────────────── */
#define DIST_ALERTA_CM   20.0f
#define BATT_MV_MAX      8400   /* 2x 18650 en serie: 4.2V × 2 */
#define BATT_MV_MIN      6000   /* 2x 18650 en serie: 3.0V × 2 */
#define BLE_MAX_FAILS    3U     /* timeouts AT consecutivos antes de reintentar */

/* Buffer global para sprintf → uart0 */
static char tx_buf[128];

/* ── Estado de errores de runtime ─────────────────────────────────────────── */
static uint8_t g_ina_error    = 0U;
static uint8_t g_ble_error    = 0U;
static uint8_t g_ble_fail_cnt = 0U;

/* ── Estado de la aspiradora ──────────────────────────────────────────────── */
typedef enum {
    VAC_EN_BASE = 0,
    VAC_LIMPIANDO,
    VAC_PAUSADO,
    VAC_VOLVIENDO,
} vac_state_t;

static vac_state_t g_vac_state    = VAC_EN_BASE;
static uint8_t     g_locate_cnt   = 0U;  /* ticks de 500ms restantes para blink locate */

/* ── Macros LED (lógica inversa: 0 = encendido) ───────────────────────────── */
#define LED_RED_TOG()   (GPIOB->PTOR = (1U << 18))
#define LED_RED_OFF()   (GPIOB->PSOR = (1U << 18))
#define LED_ALL_OFF()   do { GPIOB->PSOR = (1U<<18)|(1U<<19); GPIOD->PSOR = (1U<<1); } while(0)

/* ── Blink rojo bloqueante durante `ms` milisegundos ─────────────────────── */
static void blink_red_ms(uint32_t ms)
{
    uint32_t start = g_ms_ticks, blink = start;
    while ((g_ms_ticks - start) < ms) {
        if ((g_ms_ticks - blink) >= 500U) {
            blink += 500U;
            LED_RED_TOG();
        }
    }
    LED_RED_OFF();
}

/* ── Helpers para imprimir sin printf completo ────────────────────────────── */
static void uart0_print_int(int32_t val)
{
    char b[12]; uint8_t i = 0; uint8_t neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { uart0_send_byte('0'); return; }
    while (val > 0) { b[i++] = (char)('0' + val % 10); val /= 10; }
    if (neg) b[i++] = '-';
    for (uint8_t j = 0; j < i / 2; j++) {
        char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t;
    }
    b[i] = '\0';
    uart0_send_string(b);
}

static void uart0_print_float(float val)
{
    if (val < 0.0f) { uart0_send_byte('-'); val = -val; }
    int32_t ent = (int32_t)val;
    int32_t dec = (int32_t)((val - (float)ent) * 10.0f);
    uart0_print_int(ent); uart0_send_byte('.'); uart0_send_byte((char)('0' + dec));
}

/* ── Manejo de comandos desde el HUB vía BLE ─────────────────────────────── */
static void handle_phone_cmd(const char *cmd, const at_result_t *r)
{
    const char *at_estado = NULL;

    if (strcmp(cmd, "CMD:INICIAR") == 0) {
        g_vac_state = VAC_LIMPIANDO;
        at_estado   = "AT+ESTADO:LIMPIANDO";
        uart0_send_string("[CMD] Iniciando aspirado\r\n");
        /* TODO: activar motores via puente H */

    } else if (strcmp(cmd, "CMD:PAUSA") == 0) {
        g_vac_state = VAC_PAUSADO;
        at_estado   = "AT+ESTADO:PAUSADO";
        uart0_send_string("[CMD] Pausado\r\n");
        /* TODO: detener motores sin cambiar posicion */

    } else if (strcmp(cmd, "CMD:DETENER") == 0) {
        g_vac_state = VAC_EN_BASE;
        at_estado   = "AT+ESTADO:DETENIDO";
        uart0_send_string("[CMD] Detenido\r\n");
        /* TODO: detener motores */

    } else if (strcmp(cmd, "CMD:VOLVER") == 0) {
        g_vac_state = VAC_VOLVIENDO;
        at_estado   = "AT+ESTADO:VOLVIENDO";
        uart0_send_string("[CMD] Volviendo a base\r\n");
        /* TODO: logica de retorno al hub */

    } else if (strcmp(cmd, "CMD:LOCALIZAR") == 0) {
        g_locate_cnt = 6U;  /* 6 ticks de 500ms = 3 segundos de blink rapido */
        uart0_send_string("[CMD] Localizando...\r\n");

    } else {
        uart0_send_string("[CMD] Desconocido: ");
        uart0_send_string(cmd);
        uart0_send_string("\r\n");
    }

    if (at_estado != NULL && !g_ble_error && r->state == AT_IDLE) {
        at_send_command(at_estado);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
int main(void)
{
    bsp_init();
    ultrasonico_init();

    uart0_send_string("\r\n=== VERIFICANDO HARDWARE ===\r\n");

    /* ── 1. INA3221: reintentar hasta detectar ────────────────────────────── */
    while (!INA3221_check()) {
        uart0_send_string("[ERROR] INA3221 no detectado\r\n");
        blink_red_ms(2000U);
    }
    INA3221_init();
    uart0_send_string("[OK] INA3221\r\n");

    /* ── 2. BLE: reintentar hasta responder ───────────────────────────────── */
    at_manager_init();
    while (!at_wait_init()) {
        uart0_send_string("[ERROR] Bluetooth sin respuesta\r\n");
        blink_red_ms(2000U);
        at_manager_init();
    }
    uart0_send_string("[OK] Bluetooth\r\n");

    uart0_send_string("=== ASPIRADORA INICIADA ===\r\n");

    for (;;)
    {
        /* ── 1. Motor AT / BLE ─────────────────────────────────────────── */
        at_manager_tick();
        const at_result_t *r = at_get_result();

        if (r->state == AT_DONE) {
            /* Respuesta recibida: resetear contador de fallos */
            g_ble_fail_cnt = 0U;
            if (g_ble_error) {
                g_ble_error = 0U;
                LED_ALL_OFF();
                uart0_send_string("[OK] Bluetooth recuperado\r\n");
            }
            if (r->rssi != -100) {
                uart0_send_string("BLE RSSI: ");
                uart0_print_int(r->rssi);
                uart0_send_string(" dBm\r\n");
            }
            if (r->inq_done) uart0_send_string("BLE INQ OK\r\n");
            at_clear();

        } else if (r->state == AT_TIMEOUT) {
            uart0_send_string("AT timeout\r\n");
            g_ble_fail_cnt++;

            if (g_ble_fail_cnt >= BLE_MAX_FAILS) {
                g_ble_error    = 1U;
                g_ble_fail_cnt = 0U;
                uart0_send_string("[ERROR] Bluetooth perdido, reintentando...\r\n");
                at_manager_init();
                if (at_wait_init()) {
                    g_ble_error = 0U;
                    LED_ALL_OFF();
                    uart0_send_string("[OK] Bluetooth recuperado\r\n");
                }
                /* Si sigue sin responder: g_ble_error=1, LED blink en flag_500msec */
            }
            at_clear();
        }

        /* Comandos recibidos desde el HUB vía BLE → ESP32-C3 → UART1 */
        if (r->phone_cmd_ready) {
            handle_phone_cmd(r->phone_cmd, r);
            at_phone_cmd_clear();
        }

        /* ── 2. Motor ultrasónico (state machine, tick cada iteración) ─── */
        ultrasonico_tick();

        if (ultrasonico_is_ready()) {
            float dist = ultrasonico_get_result();
            uart0_send_string("Dist: ");
            if (dist < 0.0f) {
                uart0_send_string("---\r\n");
            } else {
                uart0_print_float(dist);
                uart0_send_string(" cm");
                if (dist < DIST_ALERTA_CM) {
                    uart0_send_string(" [!OBSTACULO!]");
                    if (!g_ble_error && r->state == AT_IDLE)
                        at_send_command("AT+ALERT:OBSTACULO");
                }
                uart0_send_string("\r\n");
            }
        }

        /* 500 ms: nueva medición + blinks de estado/error */
        if (flag_500msec) {
            flag_500msec = 0U;
            ultrasonico_start();

            if (g_locate_cnt > 0U) {
                g_locate_cnt--;
                LED_RED_TOG();  /* blink rápido durante localización */
            } else if (g_ina_error || g_ble_error) {
                LED_RED_TOG();
            }
        }

        /* ── 3. Motor de energía INA3221 (cada 1 segundo) ──────────────── */
        if (flag_1sec) {
            flag_1sec = 0U;

            if (!INA3221_check()) {
                /* Sensor perdido */
                if (!g_ina_error) {
                    g_ina_error = 1U;
                    uart0_send_string("[ERROR] INA3221 perdido\r\n");
                    if (!g_ble_error && r->state == AT_IDLE)
                        at_send_command("AT+ALERT:ERROR_INA");
                }
            } else {
                /* Sensor OK */
                if (g_ina_error) {
                    g_ina_error = 0U;
                    INA3221_init();
                    if (!g_ble_error)
                        LED_ALL_OFF();
                    uart0_send_string("[OK] INA3221 recuperado\r\n");
                }

                INA_Data_t ch1, ch2, ch3;
                INA3221_read_channel(1, &ch1);
                INA3221_read_channel(2, &ch2);
                INA3221_read_channel(3, &ch3);

                uart0_send_string("--- ENERGÍA ---\r\n");
                sprintf(tx_buf, "CH1 (Bateria):  %d mV | %d mA\r\n", (int)ch1.bus_mV, (int)ch1.current_mA);
                uart0_send_string(tx_buf);
                sprintf(tx_buf, "CH2 (Sensores): %d mV | %d mA\r\n", (int)ch2.bus_mV, (int)ch2.current_mA);
                uart0_send_string(tx_buf);
                sprintf(tx_buf, "CH3 (Motores):  %d mV | %d mA\r\n", (int)ch3.bus_mV, (int)ch3.current_mA);
                uart0_send_string(tx_buf);

                if (!g_ble_error && r->state == AT_IDLE) {
                    int32_t pct = ((ch1.bus_mV - BATT_MV_MIN) * 100) / (BATT_MV_MAX - BATT_MV_MIN);
                    if (pct < 0)   pct = 0;
                    if (pct > 100) pct = 100;
                    uart0_send_string("[BLE TX] Enviando bateria: ");
                    sprintf(tx_buf, "CH1=%dmV → %d%%\r\n", (int)ch1.bus_mV, (int)pct);
                    uart0_send_string(tx_buf);
                    sprintf(tx_buf, "AT+BATT:%d", (int)pct);
                    at_send_command(tx_buf);
                } else {
                    uart0_send_string("[BLE TX] Bateria NO enviada: ");
                    if (g_ble_error)        uart0_send_string("BLE en error  ");
                    if (r->state != AT_IDLE) uart0_send_string("AT ocupado");
                    uart0_send_string("\r\n");
                }
            }
        }
    }
    return 0;
}
