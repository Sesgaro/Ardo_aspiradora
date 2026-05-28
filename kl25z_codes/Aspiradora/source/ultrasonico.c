/*
 * ultrasonico.c  –  HC-SR04 no bloqueante
 *
 * La medición se distribuye en múltiples iteraciones del loop principal:
 *
 *   IDLE       → ultrasonico_start() lleva a TRIG_HIGH
 *   TRIG_HIGH  → activa TRIG, guarda tiempo de inicio
 *   TRIG_LOW   → espera 10 µs (15 ticks @ 1.5 MHz), desactiva TRIG,
 *                configura TPM0_CH4 para captura de flanco de subida
 *   WAIT_RISE  → espera CHF de rising edge (con timeout de 50 ms vía g_ms_ticks)
 *   WAIT_FALL  → cambia a falling edge, espera CHF, calcula distancia
 *   DONE       → expone resultado, vuelve a IDLE
 *
 * PTD4 en ALT4 = TPM0_CH4 (input capture, sin interrupción – polling de CHF)
 */

#include "MKL25Z4.h"
#include "ultrasonico.h"
#include "isr.h"   /* g_ms_ticks */

/* ── Pines ─────────────────────────────────────────────────────────────────── */
#define TRIG_HIGH()  (PTA->PSOR = (1U << 12))
#define TRIG_LOW()   (PTA->PCOR = (1U << 12))

/* ── Constantes ─────────────────────────────────────────────────────────────── */
#define TICKS_TO_CM      0.01143f   /* 1.5 MHz, velocidad del sonido 343 m/s */
#define TRIG_TICKS       15U        /* 10 µs × 1.5 ticks/µs                 */
#define ECHO_TIMEOUT_MS  50U        /* max ~38 ms para objeto muy lejano     */

/* Acceso al canal 4 de TPM0 */
#define CH4SC   (TPM0->CONTROLS[4].CnSC)
#define CH4V    (TPM0->CONTROLS[4].CnV)

/* ── Estado interno ─────────────────────────────────────────────────────────── */
typedef enum {
    US_IDLE = 0,
    US_TRIG_HIGH,
    US_TRIG_LOW,
    US_WAIT_RISE,
    US_WAIT_FALL,
    US_DONE
} us_state_t;

static us_state_t us_state       = US_IDLE;
static uint16_t   trig_start     = 0U;
static uint16_t   rise_cnt       = 0U;
static uint32_t   echo_timeout   = 0U;
static float      last_dist      = -1.0f;
static uint8_t    result_ready   = 0U;

/* ─────────────────────────────────────────────────────────────────────────────
 * ultrasonico_init()
 * ───────────────────────────────────────────────────────────────────────────── */
void ultrasonico_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK | SIM_SCGC5_PORTD_MASK;
    SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK;
    SIM->SOPT2 |= SIM_SOPT2_TPMSRC(1);   /* MCGFLLCLK (48 MHz) */

    /* PTA12 → GPIO salida (TRIG) */
    PORTA->PCR[12] = PORT_PCR_MUX(1);
    PTA->PDDR |= (1U << 12);
    TRIG_LOW();

    /* PTD4 → ALT4 = TPM0_CH4 (ECHO input capture) */
    PORTD->PCR[4] = PORT_PCR_MUX(4);

    /* TPM0 libre, prescaler 1:32 → 1.5 MHz, módulo máximo 16 bits */
    TPM0->SC  = 0;
    TPM0->MOD = 0xFFFFU;
    TPM0->CNT = 0;
    CH4SC     = 0;                                /* canal deshabilitado */
    TPM0->SC  = TPM_SC_PS(5) | TPM_SC_CMOD(1);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * ultrasonico_start()  –  solicita nueva medición (sólo desde US_IDLE)
 * ───────────────────────────────────────────────────────────────────────────── */
void ultrasonico_start(void)
{
    if (us_state == US_IDLE) {
        result_ready = 0U;
        us_state     = US_TRIG_HIGH;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * ultrasonico_tick()  –  avanza la máquina de estados; llamar cada loop
 * ───────────────────────────────────────────────────────────────────────────── */
void ultrasonico_tick(void)
{
    switch (us_state)
    {
        case US_IDLE:
            break;

        case US_TRIG_HIGH:
            TRIG_HIGH();
            trig_start = (uint16_t)TPM0->CNT;
            us_state   = US_TRIG_LOW;
            break;

        case US_TRIG_LOW:
            /* Esperar ≥ 10 µs (15 ticks) sin bloquear */
            if ((uint16_t)(TPM0->CNT - trig_start) >= TRIG_TICKS) {
                TRIG_LOW();
                /* Configurar CH4: rising edge capture + limpiar CHF residual */
                CH4SC          = TPM_CnSC_ELSA_MASK | TPM_CnSC_CHF_MASK;
                echo_timeout   = g_ms_ticks;
                us_state       = US_WAIT_RISE;
            }
            break;

        case US_WAIT_RISE:
            if (CH4SC & TPM_CnSC_CHF_MASK) {
                rise_cnt = (uint16_t)CH4V;
                /* Pasar a falling edge; escribir ELSB=1 y CHF=1 (lo limpia) */
                CH4SC    = TPM_CnSC_ELSB_MASK | TPM_CnSC_CHF_MASK;
                us_state = US_WAIT_FALL;
            } else if ((g_ms_ticks - echo_timeout) >= ECHO_TIMEOUT_MS) {
                CH4SC    = 0;
                last_dist = -1.0f;
                us_state  = US_DONE;
            }
            break;

        case US_WAIT_FALL:
            if (CH4SC & TPM_CnSC_CHF_MASK) {
                uint16_t fall_cnt = (uint16_t)CH4V;
                CH4SC = TPM_CnSC_CHF_MASK;  /* deshabilitar canal + limpiar CHF */
                /* Resta uint16 maneja desbordamiento del contador */
                uint16_t eco_ticks = (uint16_t)(fall_cnt - rise_cnt);
                last_dist = (float)eco_ticks * TICKS_TO_CM;
                us_state  = US_DONE;
            } else if ((g_ms_ticks - echo_timeout) >= ECHO_TIMEOUT_MS) {
                CH4SC    = 0;
                last_dist = -1.0f;
                us_state  = US_DONE;
            }
            break;

        case US_DONE:
            result_ready = 1U;
            us_state     = US_IDLE;
            break;
    }
}

/* ── Consulta de resultado ─────────────────────────────────────────────────── */
uint8_t ultrasonico_is_ready(void)  { return result_ready; }

float ultrasonico_get_result(void)
{
    result_ready = 0U;
    return last_dist;
}
