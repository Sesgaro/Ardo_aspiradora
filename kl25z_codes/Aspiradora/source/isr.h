#ifndef _ISR_H_
#define _ISR_H_

#include <stdint.h>

/* Contador de milisegundos desde el arranque (incrementado en SysTick) */
extern volatile uint32_t g_ms_ticks;

/* Flags de tiempo generados por SysTick (1 ms base) */
extern volatile uint8_t flag_50msec;
extern volatile uint8_t flag_100msec;
extern volatile uint8_t flag_500msec;
extern volatile uint8_t flag_1sec;

#endif // _ISR_H_
