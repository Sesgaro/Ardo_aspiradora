/*
 * logger.c - Lógica de comunicación serial
 * Muestra un menú al inicio y responde a las teclas '1' y '2'
 * para iniciar/detener el envío periódico de datos cada 5 segundos.
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include "logger.h"

/* ---- Variables de estado ---- */
volatile char   user_input_key;       /* Tecla recibida (escrita por ISR)   */
static uint8_t  is_menu_displayed;    /* 1 si el menú ya fue enviado        */
static uint8_t  logging_started;      /* 1 si el logging está activo        */
static uint8_t  do_log_count;         /* Contador para el período de 5 s    */

/* ---- Prototipos locales ---- */
static void display_menu(void);
static void transmit_string(const char *pdata);
static void transmit_byte(char byte);

/* ------------------------------------------------------------------
 * log_serial_data()
 * Llamar desde main() cada 1 segundo.
 * ------------------------------------------------------------------ */
void log_serial_data(void)
{
    /* Mostrar menú solo la primera vez */
    if (!is_menu_displayed)
    {
        display_menu();
        is_menu_displayed = 1;
    }

    /* Procesar tecla del usuario */
    if (user_input_key == '1')
    {
        if (!logging_started)
        {
            transmit_string("Logging iniciado\r\n");
            logging_started = 1;
        }
    }
    else if (user_input_key == '2')
    {
        if (logging_started)
        {
            transmit_string("Logging detenido\r\n");
            logging_started = 0;
        }
    }

    /* Limpiar tecla procesada */
    user_input_key = 0;

    /* Enviar dato cada 5 llamadas (cada 5 segundos) */
    if (logging_started)
    {
        if (++do_log_count >= 5U)
        {
            do_log_count = 0U;
            transmit_string("KL25Z UART OK\r\n");
        }
    }
}

/* ------------------------------------------------------------------
 * display_menu()
 * Envía el menú de opciones al terminal.
 * ------------------------------------------------------------------ */
static void display_menu(void)
{
    transmit_string("\r\n=== KL25Z UART Logger ===\r\n");
    transmit_string("1. Iniciar Logging\r\n");
    transmit_string("2. Detener Logging\r\n");
    transmit_string("Presiona 1 o 2...\r\n");
}

/* ------------------------------------------------------------------
 * transmit_byte()
 * Envía un solo byte por UART0 (polling: espera que el buffer quede
 * libre antes de escribir el siguiente byte).
 * ------------------------------------------------------------------ */
static void transmit_byte(char byte)
{
    /* Esperar hasta que el registro de transmisión esté libre */
    while (!(UART0->S1 & UART_S1_TDRE_MASK)) { }
    UART0->D = (uint8_t)byte;
}

/* ------------------------------------------------------------------
 * transmit_string()
 * Envía una cadena de caracteres terminada en '\0'.
 * ------------------------------------------------------------------ */
static void transmit_string(const char *pdata)
{
    while (*pdata)
    {
        transmit_byte(*pdata++);
    }
}
