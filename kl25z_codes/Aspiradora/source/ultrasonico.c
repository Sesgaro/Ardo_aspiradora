/*
 * ultrasonico.c
 * -------------
 * Módulo para sensor ultrasónico HC-SR04.
 */

#include "MKL25Z4.h"
#include "ultrasonico.h"

/* ---- Macros de acceso a pines ---- */
#define TRIG_HIGH()  (PTA->PSOR = (1U << 12))
#define TRIG_LOW()   (PTA->PCOR = (1U << 12))
#define ECHO_READ()  (PTD->PDIR & (1U << 4))

/* <-- CORRECCIÓN: Nuevo factor de conversión ticks → cm para reloj a 48 MHz */
#define TICKS_TO_CM  0.01143f

/* Tiempo máximo de espera para el pulso ECHO (~38 ms = objeto muy lejos) */
#define ECHO_TIMEOUT_TICKS  25000U

/* ------------------------------------------------------------------
 * delay_us_tpm0()
 * ------------------------------------------------------------------ */
static void delay_us_tpm0(uint16_t n)
{
    TPM0->CNT = 0;
    while (TPM0->CNT < (uint16_t)(n * 1.5f)) { }
}

/* ------------------------------------------------------------------
 * ultrasonico_init()
 * ------------------------------------------------------------------ */
void ultrasonico_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK | SIM_SCGC5_PORTD_MASK;
    SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK;
    SIM->SOPT2 |= SIM_SOPT2_TPMSRC(1);

    PORTA->PCR[12] = PORT_PCR_MUX(1);
    PTA->PDDR |= (1U << 12);
    TRIG_LOW();

    PORTD->PCR[4] = PORT_PCR_MUX(1);
    PTD->PDDR &= ~(1U << 4);

    TPM0->SC  = 0;
    TPM0->MOD = 0xFFFF;
    TPM0->CNT = 0;
    TPM0->SC  = TPM_SC_PS(5);
}

/* ------------------------------------------------------------------
 * ultrasonico_medir()
 * ------------------------------------------------------------------ */
float ultrasonico_medir(void)
{
    uint16_t eco_ticks;
    uint32_t timeout;

    TRIG_HIGH();
    delay_us_tpm0(10);
    TRIG_LOW();

    timeout = ECHO_TIMEOUT_TICKS;
    while (!ECHO_READ())
    {
        if (--timeout == 0) return -1.0f;
    }

    TPM0->CNT = 0;
    timeout   = ECHO_TIMEOUT_TICKS;

    while (ECHO_READ())
    {
        if (--timeout == 0) return -1.0f;
    }

    eco_ticks = (uint16_t)TPM0->CNT;

    return (float)eco_ticks * TICKS_TO_CM;
}
