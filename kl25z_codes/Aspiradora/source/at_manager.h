#ifndef _AT_MANAGER_H_
#define _AT_MANAGER_H_

#include <stdint.h>

/*
 * at_manager.h
 * ------------
 * Gestiona la comunicación entre la KL25Z y la ESP32 SuperMini
 * usando el protocolo AT definido en el firmware de la ESP32.
 */

#define AT_RESP_MAX_LEN  64

typedef enum {
    AT_IDLE = 0,
    AT_WAITING,
    AT_DONE,
    AT_TIMEOUT
} at_state_t;

typedef struct {
    at_state_t  state;
    char        response[AT_RESP_MAX_LEN];
    int16_t     rssi;
    uint8_t     inq_done;
} at_result_t;

void at_manager_init(void);
void at_manager_tick(void);
void at_send_command(const char *cmd);
const at_result_t *at_get_result(void);
void at_clear(void);

extern volatile char user_input_key;

#endif // _AT_MANAGER_H_
