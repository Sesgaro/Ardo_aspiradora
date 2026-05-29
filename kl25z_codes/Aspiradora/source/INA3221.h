#ifndef _INA3221_H_
#define _INA3221_H_

#include <stdint.h>

/* Dirección I2C (A0=GND, A1=GND) */
#define INA3221_ADDR                0x40U

/* Registros */
#define INA3221_REG_CONFIG          0x00U
#define INA3221_REG_SHUNTVOLTAGE_1  0x01U
#define INA3221_REG_BUSVOLTAGE_1    0x02U
#define INA3221_REG_SHUNTVOLTAGE_2  0x03U
#define INA3221_REG_BUSVOLTAGE_2    0x04U
#define INA3221_REG_SHUNTVOLTAGE_3  0x05U
#define INA3221_REG_BUSVOLTAGE_3    0x06U

/* Config: CH1+CH2+CH3 habilitados, AVG=1, VBUS_CT=1.1ms, VSH_CT=1.1ms, Continuo */
#define INA3221_CONFIG_DEFAULT      0x7127U
#define INA3221_REG_MANUFACTURER_ID 0xFEU   /* siempre lee 0x5449 ("TI") */

typedef struct {
    int32_t bus_mV;
    int32_t shunt_uV;
    int32_t current_mA;
} INA_Data_t;

uint8_t INA3221_check(void);        /* 1 = detectado, 0 = ausente/error */
void    INA3221_init(void);
void    INA3221_read_channel(uint8_t channel, INA_Data_t *data);

#endif /* _INA3221_H_ */
