
#include "MKL25Z4.h"
#include <stdint.h>
#include "BSPInit.h"
#include "at_manager.h"
#include "isr.h"

/* ---- Período de AT+INQ (cada N ciclos de AT+RSSI) ---- */
#define INQ_EVERY_N_PINGS   3U   /* INQ cada 3 × 10 s = 30 s */

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

/* Refleja el RSSI en el color del LED */
static void rssi_to_led(int16_t rssi)
{
    leds_off();
    if      (rssi > -60)  { LED_GREEN_ON(); }   /* Señal fuerte */
    else if (rssi > -80)  { LED_BLUE_ON();  }   /* Señal media  */
    else                  { LED_RED_ON();   }   /* Señal débil  */
}

/* ---- Variables de aplicación ---- */
static uint8_t ping_count = 0;   /* Cuenta pings para disparar INQ */

int main(void)
{
    bsp_init();          /* Reloj, GPIO, SysTick, UART0       */
    at_manager_init();   /* Verificación inicial con "AT"     */

    for (;;)
    {
        /* Motor de la comunicación AT (consume ISR + timeout) */
        at_manager_tick();

        const at_result_t *r = at_get_result();

        /* ---- Procesar respuesta completa ---- */
        if (r->state == AT_DONE)
        {
            if (r->rssi != -100)
            {
                /* Respuesta a AT+RSSI: reflejar señal en LED */
                rssi_to_led(r->rssi);
            }

            if (r->inq_done)
            {
                /* Respuesta a AT+INQ: escaneo BLE completado en la ESP32 */
                leds_off();
                LED_GREEN_ON();
                LED_BLUE_ON();   /* Cian = INQ completado */
            }

            at_clear();
        }

        /* ---- Timeout: LED rojo+azul (magenta) ---- */
        if (r->state == AT_TIMEOUT)
        {
            leds_off();
            LED_RED_ON();
            LED_BLUE_ON();   /* Magenta = sin respuesta */
            at_clear();
        }

        /* ---- Tarea de 500 ms: parpadeo watchdog + lanzar INQ ---- */
        if (flag_500msec)
        {
            flag_500msec = 0U;

            /* El at_manager maneja el ping automático en flag_1sec.
             * Aquí solo verificamos si toca lanzar un AT+INQ. */
            if (r->state == AT_IDLE)
            {
                if (++ping_count >= INQ_EVERY_N_PINGS * 20U) /* ×20 porque flag_500msec es cada 0.5 s × 20 = 10 s × N */
                {
                    ping_count = 0;
                    at_send_command("AT+INQ");
                }
            }
        }
    }

    return 0;
}
