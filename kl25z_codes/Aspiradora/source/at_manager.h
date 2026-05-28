#ifndef _AT_MANAGER_H_
#define _AT_MANAGER_H_

#include <stdint.h>

/*
 * at_manager.h
 * ------------
 * Gestiona la comunicación UART1 entre la KL25Z y la ESP32 SuperMini.
 *
 * Comandos AT que la KL25Z puede enviar:
 *   AT            → OK
 *   AT+RSSI       → OK+RSSI:<dBm>   o  ERR+RSSI_READ
 *   AT+INQ        → OK+INQ_DONE
 *   AT+BATT:<pct> → OK+NOTIFIED     (ESP32 reenvía "BATERIA:XX%" por BLE)
 *   AT+ALERT:<msg>→ OK+NOTIFIED     (ESP32 reenvía "ALERTA:msg"  por BLE)
 *
 * Comandos del celular (llegan cuando la ESP32 reenvía escrituras GATT):
 *   Cualquier texto terminado en \r\n → almacenado en phone_cmd
 */

#define AT_RESP_MAX_LEN  64U

typedef enum {
    AT_IDLE = 0,
    AT_WAITING,
    AT_DONE,
    AT_TIMEOUT
} at_state_t;

typedef struct {
    at_state_t state;
    char       response[AT_RESP_MAX_LEN];
    int16_t    rssi;
    uint8_t    inq_done;
    char       phone_cmd[AT_RESP_MAX_LEN];  /* comando recibido del celular */
    uint8_t    phone_cmd_ready;             /* 1 cuando hay nuevo comando   */
} at_result_t;

void                at_manager_init(void);
void                at_manager_tick(void);
void                at_send_command(const char *cmd);
const at_result_t  *at_get_result(void);
void                at_clear(void);
void                at_phone_cmd_clear(void);

#endif /* _AT_MANAGER_H_ */
