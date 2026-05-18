/*
 * Blutu_test.c  –  Main integrado
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include "BSPInit.h"
#include "at_manager.h"
#include "isr.h"
#include "ultrasonico.h"

/* ---- Período de AT+INQ (cada N pings de AT+RSSI) ---- */
#define INQ_EVERY_N_PINGS   3U   /* INQ cada 3 × 10 s = 30 s */

/* ---- Umbral de distancia para alerta (cm) ---- */
#define DIST_ALERTA_CM      20.0f

/* ---- LEDs (lógica inversa: 0 = encendido) ---- */
#define LED_RED_ON()    (GPIOB->PCOR = (1U << 18))
#define LED_RED_OFF()   (GPIOB->PSOR = (1U << 18))
#define LED_GREEN_ON()  (GPIOB->PCOR = (1U << 19))
#define LED_GREEN_OFF() (GPIOB->PSOR = (1U << 19))
#define LED_BLUE_ON()   (GPIOD->PCOR = (1U <<  1))
#define LED_BLUE_OFF()  (GPIOD->PSOR = (1U <<  1))

static void leds_off(void)
{
    LED_RED_OFF(); LED_GREEN_OFF(); LED_BLUE_OFF();
}

/* ---- Helpers de debug por UART0 (Terminal) ---- */

// <-- CORRECCIÓN: Todas estas funciones ahora usan uart0_send...
/* Envía un entero con signo por UART0 */
static void uart0_print_int(int32_t val)
{
    char buf[12];
    uint8_t i = 0;
    uint8_t neg = 0;

    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { uart0_send_byte('0'); return; }

    while (val > 0)
    {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    if (neg) buf[i++] = '-';

    for (uint8_t j = 0; j < i / 2; j++)
    {
        char tmp   = buf[j];
        buf[j]     = buf[i - 1 - j];
        buf[i-1-j] = tmp;
    }
    buf[i] = '\0';
    uart0_send_string(buf);
}

/* Envía un float con 1 decimal por UART0 */
static void uart0_print_float(float val)
{
    if (val < 0.0f)
    {
        uart0_send_byte('-');
        val = -val;
    }
    int32_t entero   = (int32_t)val;
    int32_t decimal  = (int32_t)((val - (float)entero) * 10.0f);
    uart0_print_int(entero);
    uart0_send_byte('.');
    uart0_send_byte((char)('0' + decimal));
}

/* Refleja el RSSI en el color del LED */
static void rssi_to_led(int16_t rssi)
{
    leds_off();
    if      (rssi > -60) { LED_GREEN_ON(); }
    else if (rssi > -80) { LED_BLUE_ON();  }
    else                 { LED_RED_ON();   }
}

/* ---- Variable de aplicación ---- */
static uint8_t ping_count = 0;

/* ==================================================================
 * main()
 * ================================================================== */
int main(void)
{
    bsp_init();
    ultrasonico_init();
    at_manager_init();

    // <-- CORRECCIÓN: Cambiado de uart1 a uart0 en todo el flujo principal
    uart0_send_string("\r\n=== Blutu + Ultrasonico ===\r\n");
    uart0_send_string("UART0 -> Terminal PC @ 115200 bps\r\n");
    uart0_send_string("UART1 -> ESP32 @ 9600 bps\r\n\r\n");

    for (;;)
    {
        at_manager_tick();

        const at_result_t *r = at_get_result();

        if (r->state == AT_DONE)
        {
            if (r->rssi != -100)
            {
                rssi_to_led(r->rssi);
                uart0_send_string("RSSI: ");
                uart0_print_int(r->rssi);
                uart0_send_string(" dBm\r\n");
            }

            if (r->inq_done)
            {
                leds_off();
                LED_GREEN_ON();
                LED_BLUE_ON();
                uart0_send_string("BLE INQ completado\r\n");
            }

            at_clear();
        }

        if (r->state == AT_TIMEOUT)
        {
            leds_off();
            LED_RED_ON();
            LED_BLUE_ON();
            uart0_send_string("AT timeout\r\n");
            at_clear();
        }

        if (flag_500msec)
        {
            flag_500msec = 0U;

            float dist = ultrasonico_medir();

            uart0_send_string("Dist: ");
            if (dist < 0.0f)
            {
                uart0_send_string("---");
            }
            else
            {
                uart0_print_float(dist);
                uart0_send_string(" cm");

                if (dist < DIST_ALERTA_CM)
                {
                    uart0_send_string(" [!CERCA]");
                }
            }
            uart0_send_string("\r\n");

            if (r->state == AT_IDLE)
            {
                if (++ping_count >= INQ_EVERY_N_PINGS * 20U)
                {
                    ping_count = 0;
                    at_send_command("AT+INQ");
                    uart0_send_string("Enviando AT+INQ...\r\n");
                }
            }
        }
    }

    return 0;
}
