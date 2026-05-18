#ifndef _INA3221_H_
#define _INA3221_H_

#include <stdint.h>

// Dirección I2C del sensor
#define INA3221_ADDR          0x40

// Registros de los 3 Canales
#define INA3221_REG_SHUNTVOLTAGE_1 0x01
#define INA3221_REG_BUSVOLTAGE_1   0x02
#define INA3221_REG_SHUNTVOLTAGE_2 0x03
#define INA3221_REG_BUSVOLTAGE_2   0x04
#define INA3221_REG_SHUNTVOLTAGE_3 0x05
#define INA3221_REG_BUSVOLTAGE_3   0x06

// Estructura para almacenar los datos limpios de un canal
typedef struct {
    int32_t bus_mV;
    int32_t shunt_uV;
    int32_t current_mA;
} INA_Data_t;

void INA3221_init(void);
void INA3221_read_channel(uint8_t channel, INA_Data_t *data);

#endif /* _INA3221_H_ */
