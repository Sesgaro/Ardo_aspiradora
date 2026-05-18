#include "INA3221.h"
#include "I2C.h"

// Función privada para leer un registro de 16 bits del sensor
static uint16_t INA3221_ReadRegister(uint8_t reg) {
    uint8_t rawData[2];
    int count = 0;
    if (I2C1_burstRead(INA3221_ADDR, reg, 2, rawData, &count) == ERR_NONE) {
        return (uint16_t)((rawData[0] << 8) | rawData[1]);
    }
    return 0xFFFF; // Retorna 0xFFFF si hay error en I2C
}

void INA3221_init(void) {
}

// Lee un canal específico (1, 2 o 3) y guarda los resultados en la estructura
void INA3221_read_channel(uint8_t channel, INA_Data_t *data) {
    uint8_t reg_shunt, reg_bus;

    // Seleccionamos los registros dependiendo del canal solicitado
    switch (channel) {
        case 1:
            reg_shunt = INA3221_REG_SHUNTVOLTAGE_1;
            reg_bus   = INA3221_REG_BUSVOLTAGE_1;
            break;
        case 2:
            reg_shunt = INA3221_REG_SHUNTVOLTAGE_2;
            reg_bus   = INA3221_REG_BUSVOLTAGE_2;
            break;
        case 3:
            reg_shunt = INA3221_REG_SHUNTVOLTAGE_3;
            reg_bus   = INA3221_REG_BUSVOLTAGE_3;
            break;
        default:
            return; // Canal inválido
    }

    // Leemos los registros crudos
    uint16_t busVoltageRaw   = INA3221_ReadRegister(reg_bus);
    uint16_t shuntVoltageRaw = INA3221_ReadRegister(reg_shunt);

    // Ajuste de formato (los 3 últimos bits no se usan)
    int16_t bus_shifted   = (int16_t)busVoltageRaw >> 3;
    int16_t shunt_shifted = (int16_t)shuntVoltageRaw >> 3;

    // Conversión a valores reales
    data->bus_mV     = bus_shifted * 8;
    data->shunt_uV   = shunt_shifted * 40;
    
    // Cálculo de corriente asumiendo la resistencia shunt de 100 mOhm (0.1 Ohm)
    // I = V/R -> mA = uV / 100
    data->current_mA = data->shunt_uV / 100;
}
