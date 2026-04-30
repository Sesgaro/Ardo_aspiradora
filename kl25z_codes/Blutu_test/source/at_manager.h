#ifndef _AT_MANAGER_H_
#define _AT_MANAGER_H_

#include <stdint.h>

/*
 * at_manager.h
 * ------------
 * Gestiona la comunicación entre la KL25Z y la ESP32 SuperMini
 * usando el protocolo AT definido en el firmware de la ESP32.
 *
 * Comandos soportados (según el código de la ESP32):
 *   AT            → OK
 *   AT+RSSI       → OK+RSSI:<valor>   (RSSI del mejor beacon BLE visto)
 *   AT+INQ        → OK+INQ_DONE       (dispara un escaneo BLE de 2 s)
 *
 * Flujo de uso:
 *   1. Llamar at_manager_init() una sola vez en setup.
 *   2. Llamar at_manager_tick() en cada iteración del bucle principal.
 *   3. Opcionalmente llamar at_send_command() para enviar un comando
 *      manual desde cualquier parte del código.
 */

/* Longitud máxima de la respuesta que se almacena en rx_response[] */
#define AT_RESP_MAX_LEN  64

/* Estados internos de la máquina de estados AT */
typedef enum {
    AT_IDLE = 0,   /* Sin operación pendiente                    */
    AT_WAITING,    /* Comando enviado, esperando respuesta        */
    AT_DONE,       /* Respuesta completa recibida                 */
    AT_TIMEOUT     /* No llegó respuesta en AT_TIMEOUT_MS         */
} at_state_t;

/* Resultado de la última operación AT */
typedef struct {
    at_state_t  state;                   /* Estado actual              */
    char        response[AT_RESP_MAX_LEN]; /* Texto de la respuesta    */
    int16_t     rssi;                    /* RSSI parseado (AT+RSSI)    */
    uint8_t     inq_done;                /* 1 si AT+INQ completó       */
} at_result_t;

/*
 * Inicializa el módulo. Envía "AT" para verificar la conexión.
 * Debe llamarse dentro de bsp_init() o al final del setup,
 * con interrupciones ya habilitadas.
 */
void at_manager_init(void);

/*
 * Motor de la máquina de estados. Llamar cada ciclo del bucle principal.
 * Internamente consume user_input_key (escrito por UART0_IRQHandler),
 * ensambla la respuesta caracter a caracter y la parsea al completarse.
 */
void at_manager_tick(void);

/*
 * Envía un comando AT a la ESP32 (agrega \r\n automáticamente).
 * Solo válido cuando at_get_result()->state == AT_IDLE.
 * Ejemplo: at_send_command("AT+RSSI")
 */
void at_send_command(const char *cmd);

/*
 * Devuelve un puntero al resultado de la última operación.
 * El llamador puede leer state, response, rssi e inq_done.
 */
const at_result_t *at_get_result(void);

/*
 * Limpia el estado y deja el módulo listo para el siguiente comando.
 * Llamar después de haber procesado el resultado (AT_DONE o AT_TIMEOUT).
 */
void at_clear(void);

#endif // _AT_MANAGER_H_
