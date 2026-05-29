/*
 * at_manager.c
 * ------------
 * Máquina de estados para comunicación AT entre la KL25Z y la ESP32.
 *
 * Cambios respecto a la versión original:
 *  - flag_1sec eliminado: el ping periódico usa g_ms_ticks directamente.
 *  - user_input_key reemplazado por ring buffer (isr.h) → sin race condition.
 *  - Líneas recibidas fuera de AT_WAITING se tratan como comandos del celular.
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "at_manager.h"
#include "isr.h"

/* ── Configuración ────────────────────────────────────────────────────────── */
#define AT_TIMEOUT_MS   3000U
#define AT_PING_SEC     10U     /* segundos entre pings RSSI automáticos */

/* ── Estado interno ───────────────────────────────────────────────────────── */
static at_result_t result;
static uint32_t    send_timestamp;
static uint32_t    last_ping_ms;
static char        rx_buf[AT_RESP_MAX_LEN];
static uint8_t     rx_idx;

extern volatile uint32_t g_ms_ticks;

/* ── LEDs de estado (activo en bajo) ─────────────────────────────────────── */
#define LED_RED_ON()    (GPIOB->PCOR = (1U << 18))
#define LED_RED_OFF()   (GPIOB->PSOR = (1U << 18))
#define LED_GREEN_ON()  (GPIOB->PCOR = (1U << 19))
#define LED_GREEN_OFF() (GPIOB->PSOR = (1U << 19))
#define LED_BLUE_ON()   (GPIOD->PCOR = (1U <<  1))
#define LED_BLUE_OFF()  (GPIOD->PSOR = (1U <<  1))

static void leds_off(void) { LED_RED_OFF(); LED_GREEN_OFF(); LED_BLUE_OFF(); }

static void led_status(at_state_t state)
{
    leds_off();
    switch (state) {
        case AT_IDLE:    LED_BLUE_ON();                   break;
        case AT_WAITING: LED_RED_ON();                    break;
        case AT_DONE:    LED_GREEN_ON();                  break;
        case AT_TIMEOUT: LED_RED_ON(); LED_GREEN_ON();    break;
    }
}

/* ── UART1 TX ─────────────────────────────────────────────────────────────── */
static void uart1_send_byte(char c)
{
    while (!(UART1->S1 & UART_S1_TDRE_MASK)) { }
    UART1->D = (uint8_t)c;
}

static void uart1_send_string(const char *s)
{
    while (*s) uart1_send_byte(*s++);
}

/* ── Parseo de respuesta AT ───────────────────────────────────────────────── */
static void parse_response(void)
{
    const char *r = result.response;

    if (strncmp(r, "OK+RSSI:", 8) == 0) {
        result.rssi = (int16_t)atoi(r + 8);
        return;
    }
    if (strncmp(r, "OK+INQ_DONE", 11) == 0) {
        result.inq_done = 1U;
        return;
    }
}

/* ── API pública ──────────────────────────────────────────────────────────── */

void at_manager_init(void)
{
    memset(&result, 0, sizeof(result));
    result.state  = AT_IDLE;
    result.rssi   = -100;
    rx_idx        = 0;
    rx_buf[0]     = '\0';
    last_ping_ms  = g_ms_ticks;   /* primer ping tras AT_PING_SEC segundos */

    leds_off();
    led_status(AT_IDLE);

    at_send_command("AT");
}

void at_send_command(const char *cmd)
{
    if (result.state == AT_WAITING) return;

    memset(result.response, 0, AT_RESP_MAX_LEN);
    result.rssi     = -100;
    result.inq_done = 0U;
    rx_idx          = 0;
    rx_buf[0]       = '\0';
    result.state    = AT_WAITING;
    send_timestamp  = g_ms_ticks;

    uart1_send_string(cmd);
    uart1_send_string("\r\n");
    led_status(AT_WAITING);
}

void at_manager_tick(void)
{
    /* ── 1. Drenar ring buffer ───────────────────────────────────────────── */
    char c;
    while (uart1_rbuf_get(&c))
    {
        if (c != '\r' && c != '\n') {
            if (rx_idx < AT_RESP_MAX_LEN - 1U) {
                rx_buf[rx_idx++] = c;
                rx_buf[rx_idx]   = '\0';
            }
        } else if (c == '\n' && rx_idx > 0U) {
            if (result.state == AT_WAITING) {
                /* Respuesta a un comando AT enviado */
                strncpy(result.response, rx_buf, AT_RESP_MAX_LEN - 1U);
                result.response[AT_RESP_MAX_LEN - 1U] = '\0';
                parse_response();
                result.state = AT_DONE;
                led_status(AT_DONE);
            } else {
                /* Línea recibida sin comando pendiente = viene del celular */
                strncpy(result.phone_cmd, rx_buf, AT_RESP_MAX_LEN - 1U);
                result.phone_cmd[AT_RESP_MAX_LEN - 1U] = '\0';
                result.phone_cmd_ready = 1U;
            }
            rx_idx    = 0;
            rx_buf[0] = '\0';
        }
    }

    /* ── 2. Timeout del comando en curso ────────────────────────────────── */
    if (result.state == AT_WAITING) {
        if ((g_ms_ticks - send_timestamp) >= AT_TIMEOUT_MS) {
            result.state = AT_TIMEOUT;
            led_status(AT_TIMEOUT);
        }
    }

    /* ── 3. Ping RSSI periódico (sin flag_1sec: usa g_ms_ticks) ─────────── */
    if (result.state == AT_IDLE) {
        if ((g_ms_ticks - last_ping_ms) >= (AT_PING_SEC * 1000U)) {
            last_ping_ms = g_ms_ticks;
            at_send_command("AT+RSSI");
        }
    }
}

uint8_t at_wait_init(void)
{
    while (result.state == AT_WAITING) {
        at_manager_tick();
    }
    if (result.state == AT_DONE) {
        at_clear();
        return 1U;
    }
    return 0U;
}

const at_result_t *at_get_result(void) { return &result; }

void at_clear(void)
{
    memset(result.response, 0, AT_RESP_MAX_LEN);
    result.state    = AT_IDLE;
    result.rssi     = -100;
    result.inq_done = 0U;
    rx_idx          = 0;
    rx_buf[0]       = '\0';
    led_status(AT_IDLE);
}

void at_phone_cmd_clear(void)
{
    result.phone_cmd[0]    = '\0';
    result.phone_cmd_ready = 0U;
}
