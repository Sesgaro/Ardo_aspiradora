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

#include "BSPInit.h"
#include "at_manager.h"
#include "isr.h"
#include "ultrasonico.h"
#include "INA3221.h"

/* ── Configuración ────────────────────────────────────────────────────────── */
#define DIST_ALERTA_CM   20.0f   /* umbral de obstáculo en cm              */
#define BATT_MV_MAX      4200    /* tensión a 100 % (mV)                   */
#define BATT_MV_MIN      3000    /* tensión a   0 % (mV)                   */

/* Buffer global para sprintf → uart0 */
static char tx_buf[128];

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

/* ─────────────────────────────────────────────────────────────────────────── */
int main(void)
{
    bsp_init();
    ultrasonico_init();
    at_manager_init();
    INA3221_init();

    uart0_send_string("\r\n=== ASPIRADORA INICIADA ===\r\n");

    for (;;)
    {
        /* ── 1. Motor AT / BLE ─────────────────────────────────────────── */
        at_manager_tick();
        const at_result_t *r = at_get_result();

        if (r->state == AT_DONE) {
            if (r->rssi != -100) {
                uart0_send_string("BLE RSSI: ");
                uart0_print_int(r->rssi);
                uart0_send_string(" dBm\r\n");
            }
            if (r->inq_done) uart0_send_string("BLE INQ OK\r\n");
            at_clear();
        } else if (r->state == AT_TIMEOUT) {
            uart0_send_string("AT timeout\r\n");
            at_clear();
        }

        /* Comandos recibidos desde el celular vía BLE → ESP32 → UART1 */
        if (r->phone_cmd_ready) {
            uart0_send_string("CMD celular: ");
            uart0_send_string(r->phone_cmd);
            uart0_send_string("\r\n");
            /* TODO: parsear r->phone_cmd para controlar motores, etc. */
            at_phone_cmd_clear();
        }

        /* ── 2. Motor ultrasónico (state machine, tick cada iteración) ─── */
        ultrasonico_tick();

        /* Leer resultado cuando esté listo */
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
                    /* Notificar al celular si el canal AT está libre */
                    if (r->state == AT_IDLE) {
                        at_send_command("AT+ALERT:OBSTACULO");
                    }
                }
                uart0_send_string("\r\n");
            }
        }

        /* Disparar nueva medición cada 500 ms */
        if (flag_500msec) {
            flag_500msec = 0U;
            ultrasonico_start();
        }

        /* ── 3. Motor de energía INA3221 (cada 1 segundo) ──────────────── */
        if (flag_1sec) {
            flag_1sec = 0U;   /* único punto que consume este flag */

            INA_Data_t ch1, ch2, ch3;
            INA3221_read_channel(1, &ch1);
            INA3221_read_channel(2, &ch2);
            INA3221_read_channel(3, &ch3);

            uart0_send_string("--- ENERGÍA ---\r\n");
            sprintf(tx_buf, "CH1 (Motores):  %d mV | %d mA\r\n", (int)ch1.bus_mV, (int)ch1.current_mA);
            uart0_send_string(tx_buf);
            sprintf(tx_buf, "CH2 (Sensores): %d mV | %d mA\r\n", (int)ch2.bus_mV, (int)ch2.current_mA);
            uart0_send_string(tx_buf);
            sprintf(tx_buf, "CH3 (Batería):  %d mV | %d mA\r\n", (int)ch3.bus_mV, (int)ch3.current_mA);
            uart0_send_string(tx_buf);

            /* Enviar nivel de batería al celular si el canal está libre */
            if (r->state == AT_IDLE) {
                int32_t pct = ((ch3.bus_mV - BATT_MV_MIN) * 100) / (BATT_MV_MAX - BATT_MV_MIN);
                if (pct < 0)   pct = 0;
                if (pct > 100) pct = 100;
                sprintf(tx_buf, "AT+BATT:%d", (int)pct);
                at_send_command(tx_buf);
            }
        }
    }
    return 0;
}
