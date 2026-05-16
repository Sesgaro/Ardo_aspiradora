#include "esphome.h"
#include <esp_wifi.h>
#include <driver/i2s.h>
#include <rom/ets_sys.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static uint8_t wifi_header[] = {
    0xD0, 0x00, 0x00, 0x00,
    0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, // Destino
    0x30, 0x30, 0xF9, 0x72, 0x7E, 0x0C, // Origen
    0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, // BSSID
    0x00, 0x00
};

// 1. Inyección RAW
void send_raw_frame(const uint8_t *payload, size_t payload_len) {
    bool is_promiscuous = false;
    esp_wifi_get_promiscuous(&is_promiscuous);
    if (!is_promiscuous) esp_wifi_set_promiscuous(true);

    size_t total_size = sizeof(wifi_header) + payload_len;
    if (total_size > 1450) return;

    uint8_t packet[total_size];
    memcpy(packet, wifi_header, sizeof(wifi_header));
    memcpy(packet + sizeof(wifi_header), payload, payload_len);

    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
    esp_wifi_80211_tx(WIFI_IF_AP, packet, total_size, false);
}

// 2. Muestreo Digital por I2S con AMPLIFICADOR
void i2s_sampling_task(void *pvParameters) {
    const int chunk_size = 512;
    uint8_t audio_buffer[chunk_size];
    size_t bytes_read;

    // --- GANANCIA (Volumen) ---
    // Ajusta este valor. 8 es buen punto de partida. Si suena saturado, bájalo a 4 o 6.
    // Si sigue muy bajo (si vas a hablar de lejos), súbelo a 16 o 24.
    const int GAIN_FACTOR = 8; 

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = chunk_size,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = 2,
        .ws_io_num = 9,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = 13
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);

    while (true) {
        i2s_read(I2S_NUM_0, &audio_buffer, chunk_size, &bytes_read, portMAX_DELAY);
        
        if (bytes_read > 0) {
            
            // --- INICIO DE PROCESAMIENTO DE AUDIO (DSP) ---
            // Convertimos el arreglo de bytes en muestras de 16 bits
            int16_t *samples = (int16_t *)audio_buffer;
            int num_samples = bytes_read / 2;
            
            for (int i = 0; i < num_samples; i++) {
                // Multiplicamos por la ganancia usando un entero de 32 bits
                int32_t amplified = (int32_t)samples[i] * GAIN_FACTOR;
                
                // Clipping: Si nos pasamos del límite del audio de 16 bits, lo topamos.
                // Sin esto, un grito haría un "overflow" y sonaría como ruido de módem de los 90s.
                if (amplified > 32767) amplified = 32767;
                if (amplified < -32768) amplified = -32768;
                
                samples[i] = (int16_t)amplified;
            }
            // --- FIN DEL PROCESAMIENTO ---

            send_raw_frame(audio_buffer, bytes_read);
        }
        
        vTaskDelay(1);
    }
}

// 3. Encendido
void start_ardo_engine() {
    xTaskCreatePinnedToCore(i2s_sampling_task, "i2s_task", 4096, NULL, 5, NULL, 1);
}

// 4. Heartbeat
void send_heartbeat() {
    const char* msg = "ARDO_ALIVE";
    send_raw_frame((const uint8_t*)msg, strlen(msg));
}
