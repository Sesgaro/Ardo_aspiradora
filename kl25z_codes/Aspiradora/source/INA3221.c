#include "INA3221.h"
#include "I2C.h"

static uint16_t INA3221_ReadRegister(uint8_t reg) {
    uint8_t raw[2];
    int cnt = 0;
    if (I2C1_burstRead(INA3221_ADDR, reg, 2, raw, &cnt) == ERR_NONE) {
        return (uint16_t)((raw[0] << 8) | raw[1]);
    }
    return 0xFFFFU;
}

uint8_t INA3221_check(void)
{
    uint16_t id = INA3221_ReadRegister(INA3221_REG_MANUFACTURER_ID);
    return (id == 0x5449U) ? 1U : 0U;
}

void INA3221_init(void) {
    /* Escribe la configuración por defecto para verificar comunicación y
     * dejar el sensor en estado conocido (reemplaza el power-on reset). */
    I2C1_write_reg16(INA3221_ADDR, INA3221_REG_CONFIG, INA3221_CONFIG_DEFAULT);
}

void INA3221_read_channel(uint8_t channel, INA_Data_t *data) {
    uint8_t reg_shunt, reg_bus;

    switch (channel) {
        case 1: reg_shunt = INA3221_REG_SHUNTVOLTAGE_1; reg_bus = INA3221_REG_BUSVOLTAGE_1; break;
        case 2: reg_shunt = INA3221_REG_SHUNTVOLTAGE_2; reg_bus = INA3221_REG_BUSVOLTAGE_2; break;
        case 3: reg_shunt = INA3221_REG_SHUNTVOLTAGE_3; reg_bus = INA3221_REG_BUSVOLTAGE_3; break;
        default: return;
    }

    uint16_t busRaw   = INA3221_ReadRegister(reg_bus);
    uint16_t shuntRaw = INA3221_ReadRegister(reg_shunt);

    /* Bits [2:0] no se usan (alinear a la derecha) */
    int16_t bus_s   = (int16_t)busRaw   >> 3;
    int16_t shunt_s = (int16_t)shuntRaw >> 3;

    data->bus_mV     = (int32_t)bus_s   * 8;     /* LSB = 8 mV   */
    data->shunt_uV   = (int32_t)shunt_s * 40;    /* LSB = 40 µV  */
    data->current_mA = data->shunt_uV   / 100;   /* Rshunt = 100 mΩ → I = V/R */
}
