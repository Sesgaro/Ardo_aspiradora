/*
 * at_manager.c
 * ------------
 * Máquina de estados para comunicación AT entre la KL25Z y la ESP32.
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "at_manager.h"
#include "isr.h"

/* ---- Configuración ---- */
#define AT_TIMEOUT_MS   3000U
#define AT_PING_SEC     10U

volatile char user_input_key = 0;

/* ---- Variables internas ---- */
static at_result_t  result;
static uint32_t     send_timestamp;
static uint8_t      rx_idx;
static uint8_t      ping_counter;

extern volatile uint32_t g_ms_ticks;

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
        case AT_IDLE:     LED_BLUE_ON();  break;
        case AT_WAITING:  LED_RED_ON();   break;
        case AT_DONE:     LED_GREEN_ON(); break;
        case AT_TIMEOUT:  LED_RED_ON();
                          LED_GREEN_ON(); break;
    }
}

static void uart1_send_byte(char c)
{
    while (!(UART1->S1 & UART_S1_TDRE_MASK)) { }
    UART1->D = (uint8_t)c;
}

static void uart1_send_string(const char *s)
{
    while (*s) uart1_send_byte(*s++);
}

static void parse_response(void)
{
    const char *resp = result.response;

    if (strncmp(resp, "OK+RSSI:", 8) == 0)
    {
        result.rssi = (int16_t)atoi(resp + 8);
        return;
    }

    if (strncmp(resp, "OK+INQ_DONE", 11) == 0)
    {
        result.inq_done = 1;
        return;
    }
}

void at_manager_init(void)
{
    memset(&result, 0, sizeof(result));
    result.state = AT_IDLE;
    rx_idx       = 0;
    ping_counter = 0;

    leds_off();
    led_status(AT_IDLE);

    at_send_command("AT");
}

void at_send_command(const char *cmd)
{
    if (result.state == AT_WAITING) return;

    memset(result.response, 0, AT_RESP_MAX_LEN);
    result.rssi     = -100;
    result.inq_done = 0;
    rx_idx          = 0;
    result.state    = AT_WAITING;

    send_timestamp = g_ms_ticks;

    uart1_send_string(cmd);
    uart1_send_string("\r\n");

    led_status(AT_WAITING);
}

void at_manager_tick(void)
{
    if (user_input_key != 0)
    {
        char c = user_input_key;
        user_input_key = 0;

        if (result.state == AT_WAITING)
        {
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
                parse_response();
                result.state = AT_DONE;
                led_status(AT_DONE);
            }
        }
    }

    if (result.state == AT_WAITING)
    {
        if ((g_ms_ticks - send_timestamp) >= AT_TIMEOUT_MS)
        {
            result.state = AT_TIMEOUT;
            led_status(AT_TIMEOUT);
        }
    }

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
