/*
 * isr.c - Manejadores de interrupción
 * SysTick : incrementa g_ms_ticks y genera flags de 50/100/500 ms y 1 s
 * UART1   : captura byte recibido en user_input_key (ESP32)
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include "isr.h"
#include "logger.h"

/* ---- Contador global de milisegundos (usado por at_manager) ---- */
volatile uint32_t g_ms_ticks = 0;

/* ---- Contadores internos de tiempo ---- */
static uint16_t base_tick;
static uint8_t  tick_50msec;
static uint8_t  tick_100msec;
static uint8_t  tick_500msec;

/* ---- Flags de tiempo (visibles desde main) ---- */
volatile uint8_t flag_50msec;
volatile uint8_t flag_100msec;
volatile uint8_t flag_500msec;
volatile uint8_t flag_1sec;

/* ------------------------------------------------------------------
 * SysTick_Handler()
 * Se ejecuta cada 1 ms. Incrementa g_ms_ticks y genera flags.
 * ------------------------------------------------------------------ */
void SysTick_Handler(void)
{
    g_ms_ticks++;   /* Contador absoluto de ms para timeouts */

    /* Base de 1 ms → 50 ms */
    if (++base_tick >= 50U)
    {
        base_tick    = 0U;
        flag_50msec  = 1U;

        /* 50 ms × 2 → 100 ms */
        if (++tick_50msec >= 2U)
        {
            tick_50msec   = 0U;
            flag_100msec  = 1U;

            /* 100 ms × 5 → 500 ms */
            if (++tick_100msec >= 5U)
            {
                tick_100msec  = 0U;
                flag_500msec  = 1U;

                /* 500 ms × 2 → 1 s */
                if (++tick_500msec >= 2U)
                {
                    tick_500msec = 0U;
                    flag_1sec    = 1U;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------
 * UART1_IRQHandler() // <-- CORRECCIÓN: Cambiado de UART0 a UART1
 * Se ejecuta cuando llega un byte por UART1 (ESP32).
 * Lee el dato del registro D y lo guarda en user_input_key.
 * ------------------------------------------------------------------ */
void UART1_IRQHandler(void)
{
    // <-- CORRECCIÓN: Apuntando a los registros del UART1
    if (UART1->S1 & UART_S1_RDRF_MASK)
    {
        user_input_key = (char)UART1->D;
    }
}
