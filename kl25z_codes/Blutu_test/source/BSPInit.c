#include "MKL25Z4.h"
#include <stdint.h>
#include "BSPInit.h"

#define SYSTEM_CLOCK_FREQ     48000000UL
#define BUS_CLOCK_FREQ        24000000UL

#define SYSTICK_INTERVAL_MS   1U
#define SYSTICK_RELOAD        ((SYSTEM_CLOCK_FREQ / (SYSTICK_INTERVAL_MS * 1000U)) - 1UL)

static void config_sys_clock(void);
static void gpio_init(void);
static void systick_config(void);

void bsp_init(void) {
    __disable_irq();

    config_sys_clock();
    gpio_init();
    systick_config();
    uart0_debug_init();    // <-- Ahora UART0 es la Terminal (115200 bps, PTA1/PTA2)
    uart1_esp32_init();    // <-- Ahora UART1 es el ESP32 (9600 bps, PTE0/PTE1)

    __enable_irq();
}

static void config_sys_clock(void) {
    MCG->C1 |= MCG_C1_CLKS(0) | MCG_C1_IREFS(1);
    MCG->C4 |= MCG_C4_DRST_DRS(1) | MCG_C4_DMX32(1);
}

static void gpio_init(void) {
    SIM->SCGC5 |= SIM_SCGC5_PORTB(1) | SIM_SCGC5_PORTD(1);
    PORTB->PCR[18] = PORT_PCR_MUX(1);
    PORTB->PCR[19] = PORT_PCR_MUX(1);
    PORTD->PCR[1]  = PORT_PCR_MUX(1);
    GPIOB->PDDR |= (1U << 18) | (1U << 19);
    GPIOD->PDDR |= (1U << 1);
    GPIOB->PDOR |= (1U << 18) | (1U << 19);
    GPIOD->PDOR |= (1U << 1);
}

static void systick_config(void) {
    SysTick->LOAD = (uint32_t)SYSTICK_RELOAD;
    NVIC_SetPriority(SysTick_IRQn, (1UL << __NVIC_PRIO_BITS) - 1UL);
    SysTick->VAL  = 0UL;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

/* --- UART0 PARA LA TERMINAL (115200 bps) --- */
void uart0_debug_init(void) {
    SIM->SOPT2 |= SIM_SOPT2_UART0SRC(1); // Reloj de 48 MHz
    SIM->SCGC4 |= SIM_SCGC4_UART0(1);
    SIM->SCGC5 |= SIM_SCGC5_PORTA(1);

    PORTA->PCR[1] = PORT_PCR_MUX(2); // PTA1 -> RX
    PORTA->PCR[2] = PORT_PCR_MUX(2); // PTA2 -> TX

    UART0->C2 &= ~(UART_C2_TE_MASK | UART_C2_RE_MASK);

    // BR = 48MHz / (16 * 115200) = 26 = 0x1A
    UART0->BDH = 0x00;
    UART0->BDL = 0x1A;
    UART0->C1 = 0x00;

    // Solo habilitamos TX y RX, SIN interrupciones (Polling)
    UART0->C2 = UART_C2_TE(1) | UART_C2_RE(1);
}

/* --- UART1 PARA EL ESP32 (9600 bps) --- */
void uart1_esp32_init(void) {
    SIM->SCGC4 |= SIM_SCGC4_UART1(1);
    SIM->SCGC5 |= SIM_SCGC5_PORTE(1);

    PORTE->PCR[0] = PORT_PCR_MUX(3); // PTE0 -> TX
    PORTE->PCR[1] = PORT_PCR_MUX(3); // PTE1 -> RX

    UART1->C2 &= ~(UART_C2_TE_MASK | UART_C2_RE_MASK);

    // Reloj Bus 24MHz. BR = 24MHz / (16 * 9600) = 156 = 0x9C
    UART1->BDH = 0x00;
    UART1->BDL = 0x9C;
    UART1->C1 = 0x00;

    // Habilitamos TX, RX y la Interrupción de Recepción (RIE)
    UART1->C2 = UART_C2_RIE(1) | UART_C2_TE(1) | UART_C2_RE(1);
    NVIC_EnableIRQ(UART1_IRQn);
}

/* --- RUTINAS DE ENVÍO PARA LA TERMINAL (UART0) --- */
void uart0_send_byte(char c) {
    while (!(UART0->S1 & UART_S1_TDRE_MASK));
    UART0->D = (uint8_t)c;
}

void uart0_send_string(const char *s) {
    while (*s) uart0_send_byte(*s++);
}
