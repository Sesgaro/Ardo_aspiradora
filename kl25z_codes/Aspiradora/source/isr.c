/*
 * isr.c - Manejadores de interrupción
 * SysTick  : incrementa g_ms_ticks y genera flags de 50/100/500 ms y 1 s
 * UART1 RX : captura bytes en ring buffer (sin pérdida por race condition)
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include "isr.h"

/* ── Contador global de ms ────────────────────────────────────────────────── */
volatile uint32_t g_ms_ticks = 0;

/* ── Contadores internos de tiempo ───────────────────────────────────────── */
static uint16_t base_tick;
static uint8_t  tick_50msec;
static uint8_t  tick_100msec;
static uint8_t  tick_500msec;

/* ── Flags de tiempo (visibles desde main) ───────────────────────────────── */
volatile uint8_t flag_50msec;
volatile uint8_t flag_100msec;
volatile uint8_t flag_500msec;
volatile uint8_t flag_1sec;

/* ── Ring buffer UART1 ───────────────────────────────────────────────────── */
uart1_rbuf_t g_uart1_rx = { .head = 0, .tail = 0 };

/* ─────────────────────────────────────────────────────────────────────────── */
void SysTick_Handler(void)
{
    g_ms_ticks++;

    if (++base_tick >= 50U)
    {
        base_tick   = 0U;
        flag_50msec = 1U;

        if (++tick_50msec >= 2U)
        {
            tick_50msec  = 0U;
            flag_100msec = 1U;

            if (++tick_100msec >= 5U)
            {
                tick_100msec = 0U;
                flag_500msec = 1U;

                if (++tick_500msec >= 2U)
                {
                    tick_500msec = 0U;
                    flag_1sec    = 1U;
                }
            }
        }
    }
}

void UART1_IRQHandler(void)
{
    if (UART1->S1 & UART_S1_RDRF_MASK)
    {
        char    c    = (char)UART1->D;
        uint8_t next = (uint8_t)((g_uart1_rx.head + 1U) & (UART1_RX_BUFSIZE - 1U));
        if (next != g_uart1_rx.tail)  /* no lleno → encolar */
        {
            g_uart1_rx.buf[g_uart1_rx.head] = c;
            g_uart1_rx.head = next;
        }
        /* si lleno: descarta el byte (preferible a colgarse) */
    }
}
