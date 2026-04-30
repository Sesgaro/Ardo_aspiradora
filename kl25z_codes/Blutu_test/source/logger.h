#ifndef _LOGGER_H_
#define _LOGGER_H_

#include <stdint.h>

/*
 * Byte más reciente recibido por UART0 (escrito desde UART0_IRQHandler).
 * Consumido por at_manager en cada ciclo del bucle principal.
 */
extern volatile char user_input_key;

#endif // _LOGGER_H_
