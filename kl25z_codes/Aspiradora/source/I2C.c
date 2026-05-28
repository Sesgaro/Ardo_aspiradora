#include "MKL25Z4.h"
#include "I2C.h"
#include "isr.h"   /* g_ms_ticks para timeouts */

#define I2C_TIMEOUT_MS  5U

void I2C1_init(void) {
    SIM->SCGC4 |= SIM_SCGC4_I2C1_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;

    PORTC->PCR[10] = PORT_PCR_MUX(2);   /* SCL */
    PORTC->PCR[11] = PORT_PCR_MUX(2);   /* SDA */

    I2C1->C1 = 0;
    I2C1->S  = I2C_S_IICIF_MASK;
    I2C1->F  = 0x1C;                    /* ~125 kHz a 24 MHz bus */
    I2C1->C1 = I2C_C1_IICEN_MASK;
}

/* Genera condición STOP y deja el bus libre */
static void i2c1_stop(void) {
    I2C1->C1 &= (uint8_t)~(I2C_C1_MST_MASK | I2C_C1_TX_MASK);
}

/* Espera el flag IICIF con timeout. Limpia el flag al salir.
 * Retorna ERR_NONE o ERR_TIMEOUT (y genera STOP en timeout). */
static int i2c1_wait_iicif(void) {
    uint32_t t0 = g_ms_ticks;
    while (!(I2C1->S & I2C_S_IICIF_MASK)) {
        if ((g_ms_ticks - t0) >= I2C_TIMEOUT_MS) {
            i2c1_stop();
            return ERR_TIMEOUT;
        }
    }
    I2C1->S = I2C_S_IICIF_MASK;   /* W1C: limpiar flag */
    return ERR_NONE;
}

/* ── Lectura en ráfaga ────────────────────────────────────────────────────── */
int I2C1_burstRead(uint8_t slaveAddr, uint8_t memAddr, int byteCount, uint8_t *data, int *cnt)
{
    int retry = 100;
    *cnt = 0;

    while (I2C1->S & I2C_S_BUSY_MASK) {
        if (--retry <= 0) return ERR_BUS_BUSY;
    }

    /* START + dirección escritura */
    I2C1->C1 |= I2C_C1_TX_MASK;
    I2C1->C1 |= I2C_C1_MST_MASK;
    I2C1->D   = (uint8_t)(slaveAddr << 1);
    if (i2c1_wait_iicif() != ERR_NONE)          return ERR_TIMEOUT;
    if (I2C1->S & I2C_S_ARBL_MASK)             { i2c1_stop(); return ERR_ARB_LOST; }
    if (I2C1->S & I2C_S_RXAK_MASK)             { i2c1_stop(); return ERR_NO_ACK;   }

    /* Puntero de registro */
    I2C1->D = memAddr;
    if (i2c1_wait_iicif() != ERR_NONE)          return ERR_TIMEOUT;
    if (I2C1->S & I2C_S_RXAK_MASK)             { i2c1_stop(); return ERR_NO_ACK;   }

    /* Re-START + dirección lectura */
    I2C1->C1 |= I2C_C1_RSTA_MASK;
    I2C1->D   = (uint8_t)((slaveAddr << 1) | 1U);
    if (i2c1_wait_iicif() != ERR_NONE)          return ERR_TIMEOUT;
    if (I2C1->S & I2C_S_RXAK_MASK)             { i2c1_stop(); return ERR_NO_ACK;   }

    /* Cambiar a modo recepción */
    I2C1->C1 &= (uint8_t)~(I2C_C1_TX_MASK | I2C_C1_TXAK_MASK);
    if (byteCount == 1) I2C1->C1 |= I2C_C1_TXAK_MASK;

    (void)I2C1->D;   /* lectura ficticia para arrancar reloj */

    while (byteCount > 0) {
        if (byteCount == 1) I2C1->C1 |= I2C_C1_TXAK_MASK;
        if (i2c1_wait_iicif() != ERR_NONE)      return ERR_TIMEOUT;
        if (byteCount == 1) I2C1->C1 &= (uint8_t)~I2C_C1_MST_MASK;  /* STOP antes de leer último byte */

        *data++ = I2C1->D;
        byteCount--;
        (*cnt)++;
    }
    return ERR_NONE;
}

/* ── Escritura de registro de 16 bits ────────────────────────────────────── */
int I2C1_write_reg16(uint8_t slaveAddr, uint8_t reg, uint16_t val)
{
    int retry = 100;

    while (I2C1->S & I2C_S_BUSY_MASK) {
        if (--retry <= 0) return ERR_BUS_BUSY;
    }

    /* START + dirección escritura */
    I2C1->C1 |= I2C_C1_TX_MASK;
    I2C1->C1 |= I2C_C1_MST_MASK;
    I2C1->D   = (uint8_t)(slaveAddr << 1);
    if (i2c1_wait_iicif() != ERR_NONE)  return ERR_TIMEOUT;
    if (I2C1->S & I2C_S_RXAK_MASK)    { i2c1_stop(); return ERR_NO_ACK; }

    /* Registro */
    I2C1->D = reg;
    if (i2c1_wait_iicif() != ERR_NONE)  return ERR_TIMEOUT;
    if (I2C1->S & I2C_S_RXAK_MASK)    { i2c1_stop(); return ERR_NO_ACK; }

    /* Byte alto */
    I2C1->D = (uint8_t)(val >> 8);
    if (i2c1_wait_iicif() != ERR_NONE)  return ERR_TIMEOUT;
    if (I2C1->S & I2C_S_RXAK_MASK)    { i2c1_stop(); return ERR_NO_ACK; }

    /* Byte bajo */
    I2C1->D = (uint8_t)(val & 0xFFU);
    if (i2c1_wait_iicif() != ERR_NONE)  return ERR_TIMEOUT;
    if (I2C1->S & I2C_S_RXAK_MASK)    { i2c1_stop(); return ERR_NO_ACK; }

    i2c1_stop();
    return ERR_NONE;
}
