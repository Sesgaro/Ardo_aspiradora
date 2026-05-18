#ifndef _ULTRASONICO_H_
#define _ULTRASONICO_H_

#include <stdint.h>

/*
 * ultrasonico.h
 * -------------
 * Módulo para sensor ultrasónico HC-SR04 en la KL25Z.
 *
 * Pines usados:
 *   PTA12  →  TRIG  (salida)
 *   PTD4   →  ECHO  (entrada)
 *
 * Timer:
 *   TPM0 con prescaler 1:32 sobre el bus clock (20.97 MHz aprox.)
 *   Factor de conversión: ticks × 0.02617 = distancia en cm
 *
 * Uso:
 *   1. Llamar ultrasonico_init() una sola vez en bsp_init() o en main().
 *   2. Llamar ultrasonico_medir() para obtener la distancia en cm.
 */

/* Inicializa GPIO y TPM0 para el sensor ultrasónico.
 * Llama a esta función antes de usar ultrasonico_medir(). */
void ultrasonico_init(void);

/* Dispara una medición y retorna la distancia en centímetros.
 * Bloquea ~25 ms máximo (tiempo de vuelo + pulso de trigger). */
float ultrasonico_medir(void);

#endif /* _ULTRASONICO_H_ */
