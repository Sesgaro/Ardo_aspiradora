#ifndef _ULTRASONICO_H_
#define _ULTRASONICO_H_

#include <stdint.h>

/*
 * ultrasonico.h  –  HC-SR04 no bloqueante (state machine + TPM0 input capture)
 *
 * Pines:
 *   PTA12  →  TRIG  (salida GPIO,      ALT1)
 *   PTD4   →  ECHO  (TPM0_CH4 capture, ALT4)
 *
 * Timer:
 *   TPM0 libre, prescaler 1:32, MCGFLLCLK 48 MHz → 1.5 MHz
 *   Factor: ticks × 0.01143 = distancia en cm
 *
 * Uso (desde main loop):
 *   1. ultrasonico_init()        — una sola vez al arrancar
 *   2. ultrasonico_tick()        — llamar cada iteración del loop principal
 *   3. ultrasonico_start()       — solicitar nueva medición (típico: cada 500 ms)
 *   4. ultrasonico_is_ready()    — true cuando hay resultado disponible
 *   5. ultrasonico_get_result()  — retorna cm, o -1.0f si timeout/error
 */

void  ultrasonico_init(void);
void  ultrasonico_start(void);
void  ultrasonico_tick(void);
uint8_t ultrasonico_is_ready(void);
float ultrasonico_get_result(void);

#endif /* _ULTRASONICO_H_ */
