#ifndef _ISR_H_
#define _ISR_H_

#include <stdint.h>

/* Contador de milisegundos desde el arranque */
extern volatile uint32_t g_ms_ticks;

/* Flags de tiempo generados por SysTick (base 1 ms) */
extern volatile uint8_t flag_50msec;
extern volatile uint8_t flag_100msec;
extern volatile uint8_t flag_500msec;
extern volatile uint8_t flag_1sec;

/* ── Ring buffer para UART1 RX (ESP32 → KL25Z) ───────────────────────────── */
#define UART1_RX_BUFSIZE  32U   /* debe ser potencia de 2 */

typedef struct {
    char    buf[UART1_RX_BUFSIZE];
    volatile uint8_t head;  /* escrito por ISR */
    volatile uint8_t tail;  /* leído por main  */
} uart1_rbuf_t;

extern uart1_rbuf_t g_uart1_rx;

/* Extrae un byte del ring buffer. Retorna 1 si había dato, 0 si estaba vacío. */
static inline uint8_t uart1_rbuf_get(char *c)
{
    if (g_uart1_rx.head == g_uart1_rx.tail) return 0U;
    *c = g_uart1_rx.buf[g_uart1_rx.tail];
    g_uart1_rx.tail = (uint8_t)((g_uart1_rx.tail + 1U) & (UART1_RX_BUFSIZE - 1U));
    return 1U;
}

#endif /* _ISR_H_ */
