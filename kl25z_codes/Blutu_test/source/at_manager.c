/*
 * at_manager.c
 * ------------
 * Máquina de estados para comunicación AT entre la KL25Z y la
 * ESP32 SuperMini (firmware con BLE + UART a 9600 bps).
 *
 * Protocolo:
 *   KL25Z  →  ESP32 :  "AT+RSSI\r\n"
 *   ESP32  →  KL25Z :  "OK+RSSI:-65\r\n"
 *
 * La recepción es dirigida por interrupción (UART0_IRQHandler escribe
 * en user_input_key). Este módulo la consume en at_manager_tick().
 * La transmisión usa polling (espera TDRE antes de cada byte).
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "at_manager.h"
#include "logger.h"   /* volatile char user_input_key  */
#include "isr.h"      /* flag_1sec, flag_500msec       */

/* ---- Configuración ---- */
#define AT_TIMEOUT_MS   3000U   /* ms máximos esperando respuesta     */
#define AT_PING_SEC     10U     /* segundos entre AT+RSSI automáticos */

/* ---- Variables internas ---- */
static at_result_t  result;
static uint32_t     send_timestamp;   /* millis() al enviar el comando  */
static uint8_t      rx_idx;           /* índice en result.response[]    */
static uint8_t      ping_counter;     /* contador de segundos para ping */

/* ---- Tiempo simple (incrementado desde SysTick_Handler) ---- */
extern volatile uint32_t g_ms_ticks;  /* Definido aquí, declarado en isr.h */

/* ---- LEDs (lógica inversa: 0 = encendido) ---- */
#define LED_RED_ON()    (GPIOB->PCOR = (1U << 18))
#define LED_RED_OFF()   (GPIOB->PSOR = (1U << 18))
#define LED_GREEN_ON()  (GPIOB->PCOR = (1U << 19))
#define LED_GREEN_OFF() (GPIOB->PSOR = (1U << 19))
#define LED_BLUE_ON()   (GPIOD->PCOR = (1U <<  1))
#define LED_BLUE_OFF()  (GPIOD->PSOR = (1U <<  1))

static void leds_off(void)
{
    LED_RED_OFF(); LED_GREEN_OFF(); LED_BLUE_OFF();
}

static void led_status(at_state_t state)
{
    leds_off();
    switch (state)
    {
        case AT_IDLE:     LED_BLUE_ON();  break;  /* Azul  = listo       */
        case AT_WAITING:  LED_RED_ON();   break;  /* Rojo  = esperando   */
        case AT_DONE:     LED_GREEN_ON(); break;  /* Verde = OK recibido */
        case AT_TIMEOUT:  LED_RED_ON();
                          LED_GREEN_ON(); break;  /* Amarillo = timeout  */
    }
}

/* ------------------------------------------------------------------
 * uart0_send_byte() / uart0_send_string()
 * Transmisión por polling: espera que el buffer de TX esté libre.
 * ------------------------------------------------------------------ */
static void uart0_send_byte(char c)
{
    while (!(UART0->S1 & UART_S1_TDRE_MASK)) { }
    UART0->D = (uint8_t)c;
}

static void uart0_send_string(const char *s)
{
    while (*s) uart0_send_byte(*s++);
}

/* ------------------------------------------------------------------
 * parse_response()
 * Extrae información útil de la respuesta recibida.
 * ------------------------------------------------------------------ */
static void parse_response(void)
{
    const char *resp = result.response;

    /* OK+RSSI:<número> */
    if (strncmp(resp, "OK+RSSI:", 8) == 0)
    {
        result.rssi = (int16_t)atoi(resp + 8);
        return;
    }

    /* OK+INQ_DONE */
    if (strncmp(resp, "OK+INQ_DONE", 11) == 0)
    {
        result.inq_done = 1;
        return;
    }

    /* OK (respuesta genérica de AT) */
    /* No requiere acción adicional */
}

/* ------------------------------------------------------------------
 * at_manager_init()
 * ------------------------------------------------------------------ */
void at_manager_init(void)
{
    memset(&result, 0, sizeof(result));
    result.state = AT_IDLE;
    rx_idx       = 0;
    ping_counter = 0;

    leds_off();
    led_status(AT_IDLE);

    /* Verificar conexión con la ESP32 */
    at_send_command("AT");
}

/* ------------------------------------------------------------------
 * at_send_command()
 * ------------------------------------------------------------------ */
void at_send_command(const char *cmd)
{
    if (result.state == AT_WAITING) return;  /* Ya hay un comando en vuelo */

    memset(result.response, 0, AT_RESP_MAX_LEN);
    result.rssi     = -100;
    result.inq_done = 0;
    rx_idx          = 0;
    result.state    = AT_WAITING;

    send_timestamp = g_ms_ticks;

    uart0_send_string(cmd);
    uart0_send_string("\r\n");

    led_status(AT_WAITING);
}

/* ------------------------------------------------------------------
 * at_manager_tick()
 * Llamar en cada iteración del bucle principal (o cada 10 ms).
 * ------------------------------------------------------------------ */
void at_manager_tick(void)
{
    /* --- Consumir byte de la ISR --- */
    if (user_input_key != 0)
    {
        char c = user_input_key;
        user_input_key = 0;

        if (result.state == AT_WAITING)
        {
            /* Acumular hasta \n o buffer lleno; ignorar \r */
            if (c != '\r' && c != '\n')
            {
                if (rx_idx < AT_RESP_MAX_LEN - 1)
                {
                    result.response[rx_idx++] = c;
                    result.response[rx_idx]   = '\0';
                }
            }
            else if (c == '\n' && rx_idx > 0)
            {
                /* Línea completa recibida */
                parse_response();
                result.state = AT_DONE;
                led_status(AT_DONE);
            }
        }
    }

    /* --- Timeout --- */
    if (result.state == AT_WAITING)
    {
        if ((g_ms_ticks - send_timestamp) >= AT_TIMEOUT_MS)
        {
            result.state = AT_TIMEOUT;
            led_status(AT_TIMEOUT);
        }
    }

    /* --- Ping automático cada AT_PING_SEC segundos --- */
    if (flag_1sec)
    {
        flag_1sec = 0U;

        if (result.state == AT_IDLE || result.state == AT_DONE)
        {
            at_clear();

            if (++ping_counter >= AT_PING_SEC)
            {
                ping_counter = 0;
                at_send_command("AT+RSSI");
            }
        }
    }
}

/* ------------------------------------------------------------------
 * at_get_result() / at_clear()
 * ------------------------------------------------------------------ */
const at_result_t *at_get_result(void)
{
    return &result;
}

void at_clear(void)
{
    memset(result.response, 0, AT_RESP_MAX_LEN);
    result.state    = AT_IDLE;
    result.rssi     = -100;
    result.inq_done = 0;
    rx_idx          = 0;
    led_status(AT_IDLE);
}
