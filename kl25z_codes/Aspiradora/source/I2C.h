#ifndef _I2C_H_
#define _I2C_H_

#include <stdint.h>

#define ERR_NONE              0
#define ERR_NO_ACK            0x01
#define ERR_ARB_LOST          0x02
#define ERR_BUS_BUSY          0x03

void I2C1_init(void);
int I2C1_burstRead(uint8_t slaveAddr, uint8_t memAddr, int byteCount, uint8_t* data, int* cnt);

#endif /* _I2C_H_ */
