/*
 * LEDBlink.c - Control del LED RGB en la Freedom KL25Z
 * PTB18 = LED Rojo  | PTB19 = LED Verde | PTD1 = LED Azul
 * Lógica inversa: escribir 0 enciende el LED.
 */

#include "MKL25Z4.h"
#include <stdint.h>

#define toggle_red_LED()   (GPIOB->PTOR = (1U << 18))
#define toggle_green_LED() (GPIOB->PTOR = (1U << 19))
#define toggle_blue_LED()  (GPIOD->PTOR = (1U <<  1))

/* Llamar cada 500 ms desde main() */
void control_LEDs(void)
{
    toggle_blue_LED();
}
