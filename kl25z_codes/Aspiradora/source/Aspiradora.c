/*
 * Aspiradora.c  –  Cerebro principal de la Aspiradora Inteligente
 * -------------------------------------------------------------------------
 * Integra:
 * - Comunicación ESP32 (BLE) vía UART1
 * - Sensor de Distancia HC-SR04 vía TPM0
 * - Sensor de Energía INA3221 (3 Canales) vía I2C1
 * - Terminal de Debug vía UART0
 */

#include "MKL25Z4.h"
#include <stdint.h>
#include <stdio.h>

#include "BSPInit.h"
#include "at_manager.h"
#include "isr.h"
#include "ultrasonico.h"
#include "INA3221.h"

#define INQ_EVERY_N_PINGS   3U
#define DIST_ALERTA_CM      20.0f

static uint8_t ping_count = 0;
char tx_buffer[100]; // Buffer global para enviar textos formateados

// Helpers para imprimir
static void uart0_print_int(int32_t val) {
    char buf[12]; uint8_t i = 0; uint8_t neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { uart0_send_byte('0'); return; }
    while (val > 0) { buf[i++] = (char)('0' + (val % 10)); val /= 10; }
    if (neg) buf[i++] = '-';
    for (uint8_t j = 0; j < i / 2; j++) {
        char tmp = buf[j]; buf[j] = buf[i - 1 - j]; buf[i-1-j] = tmp;
    }
    buf[i] = '\0';
    uart0_send_string(buf);
}

static void uart0_print_float(float val) {
    if (val < 0.0f) { uart0_send_byte('-'); val = -val; }
    int32_t entero = (int32_t)val;
    int32_t decimal = (int32_t)((val - (float)entero) * 10.0f);
    uart0_print_int(entero); uart0_send_byte('.'); uart0_send_byte((char)('0' + decimal));
}

int main(void) {
    bsp_init();             
    ultrasonico_init();     
    at_manager_init();      
    INA3221_init();

    uart0_send_string("\r\n=== ASPIRADORA INICIADA ===\r\n");

    for (;;) {
        // 1. MOTOR BLUETOOTH (ESP32)
        at_manager_tick();
        const at_result_t *r = at_get_result();

        if (r->state == AT_DONE) {
            if (r->rssi != -100) {
                uart0_send_string("BLE RSSI: ");
                uart0_print_int(r->rssi);
                uart0_send_string(" dBm\r\n");
            }
            if (r->inq_done) uart0_send_string("BLE INQ completado\r\n");
            at_clear();
        }

        if (r->state == AT_TIMEOUT) {
            uart0_send_string("AT timeout\r\n");
            at_clear();
        }

        // 2. MOTOR ULTRASÓNICO (Cada 500 ms)
        if (flag_500msec) {
            flag_500msec = 0U;
            float dist = ultrasonico_medir();

            uart0_send_string("Distancia: ");
            if (dist < 0.0f) uart0_send_string("---");   
            else {
                uart0_print_float(dist);
                uart0_send_string(" cm");
                if (dist < DIST_ALERTA_CM) uart0_send_string(" [!OBSTÁCULO!]");
            }
            uart0_send_string("\r\n");

            // Manejo de peticiones BLE periódicas
            if (r->state == AT_IDLE) {
                if (++ping_count >= INQ_EVERY_N_PINGS * 20U) {
                    ping_count = 0;
                    at_send_command("AT+INQ");
                }
            }
        }

        // 3. MOTOR DE ENERGÍA INA3221 (Cada 1 segundo)
        if (flag_1sec) {
            flag_1sec = 0U;
            
            INA_Data_t ch1, ch2, ch3;
            
            // Leemos los 3 canales
            INA3221_read_channel(1, &ch1);
            INA3221_read_channel(2, &ch2);
            INA3221_read_channel(3, &ch3);

            uart0_send_string("\r\n--- REPORTE DE ENERGÍA ---\r\n");
            
            sprintf(tx_buffer, "CH1 (Motores): %ld mV | %ld mA\r\n", ch1.bus_mV, ch1.current_mA);
            uart0_send_string(tx_buffer);
            
            sprintf(tx_buffer, "CH2 (Sensores): %ld mV | %ld mA\r\n", ch2.bus_mV, ch2.current_mA);
            uart0_send_string(tx_buffer);
            
            sprintf(tx_buffer, "CH3 (Carga): %ld mV | %ld mA\r\n", ch3.bus_mV, ch3.current_mA);
            uart0_send_string(tx_buffer);
            
            uart0_send_string("--------------------------\r\n\r\n");
        }
    }
    return 0;
}
