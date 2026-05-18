#include "MKL25Z4.h"
#include "I2C.h"

// --- I2C1 EN EL PUERTO C (PTC10 y PTC11) ---
void I2C1_init(void) {
    SIM->SCGC4 |= SIM_SCGC4_I2C1_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;
    
    PORTC->PCR[10] = PORT_PCR_MUX(2);         // SCL
    PORTC->PCR[11] = PORT_PCR_MUX(2);         // SDA
    
    I2C1->C1 = 0;
    I2C1->S = I2C_S_IICIF_MASK;
    I2C1->F = 0x1C; // Configuración de velocidad (baudrate)
    I2C1->C1 = I2C_C1_IICEN_MASK;
}

// --- LECTURA EN RÁFAGA (BURST READ) ---
int I2C1_burstRead(uint8_t slaveAddr, uint8_t memAddr, int byteCount, uint8_t* data, int* cnt) {
    int retry = 100;
    *cnt = 0;

    while (I2C1->S & I2C_S_BUSY_MASK) {
        if (--retry <= 0) return ERR_BUS_BUSY;
    }

    I2C1->C1 |= I2C_C1_TX_MASK;
    I2C1->C1 |= I2C_C1_MST_MASK;

    I2C1->D = slaveAddr << 1;
    while(!(I2C1->S & I2C_S_IICIF_MASK));
    I2C1->S |= I2C_S_IICIF_MASK;
    if (I2C1->S & I2C_S_ARBL_MASK) return ERR_ARB_LOST;
    if (I2C1->S & I2C_S_RXAK_MASK) return ERR_NO_ACK;

    I2C1->D = memAddr;
    while(!(I2C1->S & I2C_S_IICIF_MASK));
    I2C1->S |= I2C_S_IICIF_MASK;
    if (I2C1->S & I2C_S_RXAK_MASK) return ERR_NO_ACK;

    I2C1->C1 |= I2C_C1_RSTA_MASK;

    I2C1->D = (slaveAddr << 1) | 1;
    while(!(I2C1->S & I2C_S_IICIF_MASK));
    I2C1->S |= I2C_S_IICIF_MASK;
    if (I2C1->S & I2C_S_RXAK_MASK) return ERR_NO_ACK;

    I2C1->C1 &= ~(I2C_C1_TX_MASK | I2C_C1_TXAK_MASK);
    if (byteCount == 1) I2C1->C1 |= I2C_C1_TXAK_MASK;

    (void)I2C1->D; // Lectura falsa para iniciar reloj

    while (byteCount > 0) {
        if (byteCount == 1) I2C1->C1 |= I2C_C1_TXAK_MASK;
        while(!(I2C1->S & I2C_S_IICIF_MASK));
        I2C1->S |= I2C_S_IICIF_MASK;
        if (byteCount == 1) I2C1->C1 &= ~I2C_C1_MST_MASK;

        *data++ = I2C1->D;
        byteCount--;
        (*cnt)++;
    }
    return ERR_NONE;
}
