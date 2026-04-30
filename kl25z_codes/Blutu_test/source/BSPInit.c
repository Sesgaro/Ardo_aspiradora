/*
 * BSPInit.c - Board Support Package para KL25Z
 * UART0 configurado a 9600 baudios, 8-N-1
 * Basado en: learningmicro.wordpress.com/serial-communication-interface-using-uart
 *
 * Clock   : FLL @ 48 MHz (MCGFLLCLK)
 * UART    : UART0 | Pines PTA1 (RX) y PTA2 (TX)
 * Baud    : 9600  | OSR=16, BDH=0x01, BDL=0x38
 * Config  : 8-N-1 (8 bits de datos, sin paridad, 1 bit de stop)
 */

#include "MKL25Z4.h"
#include <stdint.h>

/* ---- Constantes ---- */
#define SYSTEM_CLOCK_FREQ     48000000UL   /* 48 MHz                  */
#define SYSTICK_INTERVAL_MS   1            /* Periodo de tick: 1 ms   */
#define SYSTICK_FREQ_HZ       (SYSTICK_INTERVAL_MS * 1000U)
#define SYSTICK_RELOAD        ((SYSTEM_CLOCK_FREQ / SYSTICK_FREQ_HZ) - 1UL)

/* ---- Prototipos locales ---- */
static void config_sys_clock(void);
static void gpio_init(void);
static void systick_config(void);
static void uart0_init(void);

/* ------------------------------------------------------------------
 * bsp_init()
 * Inicializa todos los periféricos del sistema.
 * ------------------------------------------------------------------ */
void bsp_init(void)
{
    __disable_irq();        /* Deshabilitar interrupciones globales   */

    config_sys_clock();     /* 1. Reloj del sistema a 48 MHz          */
    gpio_init();            /* 2. LEDs RGB                            */
    systick_config();       /* 3. Timer de base de tiempo (1 ms tick) */
    uart0_init();           /* 4. UART0 a 9600 bps                    */

    __enable_irq();         /* Habilitar interrupciones globales      */
}

/* ------------------------------------------------------------------
 * config_sys_clock()
 * Configura el FLL del MCG para generar 48 MHz.
 * ------------------------------------------------------------------ */
static void config_sys_clock(void)
{
    /* Seleccionar PLL/FLL como fuente de reloj del sistema */
    MCG->C1 |= MCG_C1_CLKS(0);

    /* Usar reloj interno de referencia (IRC) como entrada del FLL */
    MCG->C1 |= MCG_C1_IREFS(1);

    /* Rango medio del DCO */
    MCG->C4 |= MCG_C4_DRST_DRS(1);

    /* DCO a 48 MHz (DMX32 = 1) */
    MCG->C4 |= MCG_C4_DMX32(1);
}

/* ------------------------------------------------------------------
 * gpio_init()
 * Configura los pines del LED RGB (PTB18, PTB19, PTD1) como salidas.
 * ------------------------------------------------------------------ */
static void gpio_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB(1);  /* Habilitar reloj PORTB */
    SIM->SCGC5 |= SIM_SCGC5_PORTD(1);  /* Habilitar reloj PORTD */

    PORTB->PCR[18] = PORT_PCR_MUX(1);  /* PTB18 como GPIO (LED Rojo)  */
    PORTB->PCR[19] = PORT_PCR_MUX(1);  /* PTB19 como GPIO (LED Verde) */
    PORTD->PCR[1]  = PORT_PCR_MUX(1);  /* PTD1  como GPIO (LED Azul)  */

    GPIOB->PDDR |= (1U << 18) | (1U << 19);  /* PTB18, PTB19 como salidas */
    GPIOD->PDDR |= (1U << 1);                 /* PTD1  como salida         */

    GPIOB->PDOR |= (1U << 18) | (1U << 19);
    GPIOD->PDOR |= (1U << 1);
}

static void systick_config(void)
{
    SysTick->LOAD = (uint32_t)SYSTICK_RELOAD;
    NVIC_SetPriority(SysTick_IRQn, (1UL << __NVIC_PRIO_BITS) - 1UL);
    SysTick->VAL  = 0UL;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk    |
                    SysTick_CTRL_ENABLE_Msk;
}

/* ------------------------------------------------------------------
 * uart0_init()
 * Configura UART0 a 9600 bps, 8-N-1 con interrupción de recepción.
 *
 * Cálculo de baud rate:
 *   Baud = UART_CLK / ((OSR+1) * BR)
 *   9600 = 48_000_000 / (16 * BR)  →  BR = 312 = 0x138
 *   BDH = SBR[12:8] = 0x01
 *   BDL = SBR[7:0]  = 0x38
 * ------------------------------------------------------------------ */
static void uart0_init(void)
{
    SIM->SOPT2 |= SIM_SOPT2_UART0SRC(1);

    SIM->SCGC4 |= SIM_SCGC4_UART0(1);
    SIM->SCGC5 |= SIM_SCGC5_PORTA(1);

    PORTA->PCR[1] |= PORT_PCR_MUX(2);
    PORTA->PCR[2] |= PORT_PCR_MUX(2);

    UART0->C2 &= ~(UART_C2_TE_MASK | UART_C2_RE_MASK);

    UART0->BDH = 0x01;
    UART0->BDL = 0x38;

    UART0->C1 = 0x00;

    UART0->C2 = UART_C2_RIE(1) | UART_C2_TE(1) | UART_C2_RE(1);

    NVIC_EnableIRQ(UART0_IRQn);
}
