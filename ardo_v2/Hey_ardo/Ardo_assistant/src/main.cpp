/**
 * Ardo Assistant — main.cpp (versión diagnóstico)
 * Imprime los parámetros exactos del modelo para calibrar la normalización.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "led_strip.h"

#include "hey_ardo_model.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"

#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"

// =============================================
// PINES
// =============================================
#define I2S_BCLK_PIN     GPIO_NUM_2
#define I2S_WS_PIN       GPIO_NUM_9
#define I2S_DIN_PIN      GPIO_NUM_13
#define LED_PIN          48
#define SAMPLE_RATE      16000

// =============================================
// PARÁMETROS
// =============================================
#define NUM_MEL_CHANNELS     40
#define WINDOW_SIZE_MS       30
#define STEP_SIZE_MS         10
#define WARMUP_READS         50
#define SLIDING_WINDOW_SIZE  10
#define PROBABILITY_CUTOFF   0.15f
#define COOLDOWN_MS          1500

#define TENSOR_ARENA_SIZE    (96 * 1024)
static uint8_t* tensor_arena = nullptr;

#define I2S_QUEUE_DEPTH      4
#define I2S_BLOCK_SAMPLES    160
#define I2S_BLOCK_BYTES      (I2S_BLOCK_SAMPLES * sizeof(int16_t))
static QueueHandle_t s_i2s_queue = nullptr;

// =============================================
// HANDLES
// =============================================
static i2s_chan_handle_t         s_rx_chan    = nullptr;
static led_strip_handle_t        s_led        = nullptr;
static const esp_afe_sr_iface_t* s_afe_handle = nullptr;
static esp_afe_sr_data_t*        s_afe_data   = nullptr;
static struct FrontendConfig     s_fe_config;
static struct FrontendState      s_fe_state;

static const char* TAG = "ARDO";

// =============================================
// LED
// =============================================
static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

static void init_led() {
    led_strip_config_t cfg = {};
    cfg.strip_gpio_num = LED_PIN;
    cfg.max_leds       = 1;
    led_strip_rmt_config_t rmt = {};
    rmt.resolution_hz  = 10 * 1000 * 1000;
    led_strip_new_rmt_device(&cfg, &rmt, &s_led);
    led_strip_clear(s_led);
    led_set(0, 0, 15);
}

// =============================================
// I2S
// =============================================
static void init_microphone() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = I2S_BLOCK_SAMPLES;
    i2s_new_channel(&chan_cfg, nullptr, &s_rx_chan);

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = I2S_BCLK_PIN;
    std_cfg.gpio_cfg.ws   = I2S_WS_PIN;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din  = I2S_DIN_PIN;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    i2s_channel_enable(s_rx_chan);
    ESP_LOGI(TAG, "I2S OK");
}

// =============================================
// AFE
// =============================================
static void init_afe() {
    srmodel_list_t* models = esp_srmodel_init("model");
    afe_config_t* cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    cfg->wakenet_init = false;
    cfg->vad_init     = true;
    cfg->agc_init     = true;
    s_afe_handle = esp_afe_handle_from_config(cfg);
    s_afe_data   = s_afe_handle->create_from_config(cfg);
    ESP_LOGI(TAG, "AFE OK — feed=%d fetch=%d",
             s_afe_handle->get_feed_chunksize(s_afe_data),
             s_afe_handle->get_fetch_chunksize(s_afe_data));
}

// =============================================
// MICROFRONTEND
// =============================================
static bool init_frontend() {
    memset(&s_fe_config, 0, sizeof(s_fe_config));
    s_fe_config.window.size_ms                       = WINDOW_SIZE_MS;
    s_fe_config.window.step_size_ms                  = STEP_SIZE_MS;
    s_fe_config.filterbank.num_channels              = NUM_MEL_CHANNELS;
    s_fe_config.filterbank.lower_band_limit          = 125.0f;
    s_fe_config.filterbank.upper_band_limit          = 7500.0f;
    s_fe_config.noise_reduction.smoothing_bits       = 10;
    s_fe_config.noise_reduction.even_smoothing       = 0.025f;
    s_fe_config.noise_reduction.odd_smoothing        = 0.06f;
    s_fe_config.noise_reduction.min_signal_remaining = 0.05f;
    s_fe_config.pcan_gain_control.enable_pcan        = 1;
    s_fe_config.pcan_gain_control.strength           = 0.95f;
    s_fe_config.pcan_gain_control.offset             = 80.0f;
    s_fe_config.pcan_gain_control.gain_bits          = 21;
    s_fe_config.log_scale.enable_log                 = 1;
    s_fe_config.log_scale.scale_shift                = 6;

    if (!FrontendPopulateState(&s_fe_config, &s_fe_state, SAMPLE_RATE)) {
        ESP_LOGE(TAG, "FrontendPopulateState falló");
        return false;
    }
    ESP_LOGI(TAG, "MicFrontend OK");
    return true;
}

// =============================================
// SLIDING WINDOW
// =============================================
typedef struct {
    float buffer[SLIDING_WINDOW_SIZE];
    int   head;
    int   count;
    float sum;
} SlidingWindow;

static void sw_reset(SlidingWindow* sw) {
    memset(sw->buffer, 0, sizeof(sw->buffer));
    sw->head = sw->count = 0;
    sw->sum  = 0.0f;
}

static float sw_push(SlidingWindow* sw, float prob) {
    sw->sum -= sw->buffer[sw->head];
    sw->buffer[sw->head] = prob;
    sw->sum += prob;
    sw->head = (sw->head + 1) % SLIDING_WINDOW_SIZE;
    if (sw->count < SLIDING_WINDOW_SIZE) sw->count++;
    return sw->sum / (float)sw->count;
}

// =============================================
// TASK I2S — Core 0
// =============================================
static void i2s_reader_task(void* arg) {
    int32_t* raw32 = (int32_t*)heap_caps_malloc(
        I2S_BLOCK_SAMPLES * sizeof(int32_t), MALLOC_CAP_INTERNAL);
    int16_t* pcm16 = (int16_t*)heap_caps_malloc(
        I2S_BLOCK_BYTES, MALLOC_CAP_INTERNAL);

    if (!raw32 || !pcm16) {
        ESP_LOGE(TAG, "Sin RAM para I2S");
        vTaskDelete(nullptr); return;
    }

    for (int i = 0; i < WARMUP_READS; i++) {
        size_t br;
        i2s_channel_read(s_rx_chan, raw32,
                         I2S_BLOCK_SAMPLES * sizeof(int32_t), &br, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "Micrófono listo");
    led_set(0, 0, 5);

    while (true) {
        size_t bytes_read;
        if (i2s_channel_read(s_rx_chan, raw32,
                             I2S_BLOCK_SAMPLES * sizeof(int32_t),
                             &bytes_read, portMAX_DELAY) != ESP_OK) continue;

        int n = bytes_read / sizeof(int32_t);
        for (int i = 0; i < n; i++) {
            int32_t s = raw32[i] >> 8;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            pcm16[i] = (int16_t)s;
        }
        xQueueSend(s_i2s_queue, pcm16, 0);
    }
}

// =============================================
// TASK WAKE WORD — Core 1
// =============================================
static void wakeword_task(void* arg) {
    const tflite::Model* model = tflite::GetModel(hey_ardo_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Modelo incompatible"); vTaskDelete(nullptr); return;
    }

    tflite::MicroMutableOpResolver<16> resolver;
    resolver.AddFullyConnected();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddReshape();
    resolver.AddSoftmax();
    resolver.AddRelu();
    resolver.AddCallOnce();
    resolver.AddReadVariable();
    resolver.AddAssignVariable();
    resolver.AddVarHandle();
    resolver.AddConcatenation();
    resolver.AddStridedSlice();
    resolver.AddSplitV();
    resolver.AddLogistic();
    resolver.AddQuantize();
    resolver.AddDequantize();

    tflite::MicroAllocator* allocator =
        tflite::MicroAllocator::Create(tensor_arena, TENSOR_ARENA_SIZE);
    tflite::MicroResourceVariables* res_vars =
        tflite::MicroResourceVariables::Create(allocator, 10);
    tflite::MicroInterpreter interpreter(model, resolver, allocator, res_vars);

    if (interpreter.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors falló");
        vTaskDelete(nullptr); return;
    }

    // --- Obtener tensores ANTES del diagnóstico ---
    TfLiteTensor* input  = interpreter.input(0);
    TfLiteTensor* output = interpreter.output(0);

    // =============================================
    // DIAGNÓSTICO DEL MODELO — imprime UNA VEZ al arrancar
    // Con estos valores calibramos la normalización correcta
    // =============================================
    ESP_LOGI(TAG, "=== DIAGNÓSTICO DEL MODELO ===");
    ESP_LOGI(TAG, "Input:");
    ESP_LOGI(TAG, "  bytes=%d", input->bytes);
    ESP_LOGI(TAG, "  dims.size=%d", input->dims->size);
    for (int d = 0; d < input->dims->size; d++) {
        ESP_LOGI(TAG, "  dims[%d]=%d", d, input->dims->data[d]);
    }
    ESP_LOGI(TAG, "  type=%d (8=int8, 1=float32)", (int)input->type);
    ESP_LOGI(TAG, "  scale=%.8f", input->params.scale);
    ESP_LOGI(TAG, "  zero_point=%d", input->params.zero_point);

    ESP_LOGI(TAG, "Output:");
    ESP_LOGI(TAG, "  bytes=%d", output->bytes);
    ESP_LOGI(TAG, "  dims.size=%d", output->dims->size);
    for (int d = 0; d < output->dims->size; d++) {
        ESP_LOGI(TAG, "  dims[%d]=%d", d, output->dims->data[d]);
    }
    ESP_LOGI(TAG, "  type=%d (8=int8, 1=float32)", (int)output->type);
    ESP_LOGI(TAG, "  scale=%.8f", output->params.scale);
    ESP_LOGI(TAG, "  zero_point=%d", output->params.zero_point);
    ESP_LOGI(TAG, "==============================");

    // --- Parámetros de cuantización ---
    int   num_classes  = (int)(output->bytes / sizeof(int8_t));
    int   wake_class   = (num_classes > 1) ? 1 : 0;
    float out_scale    = output->params.scale;
    int   out_zp       = output->params.zero_point;
    float input_scale  = (input->params.scale == 0.0f) ? 1.0f : input->params.scale;

    ESP_LOGI(TAG, "Clases=%d wake_class=%d", num_classes, wake_class);

    // Spectrogram history en PSRAM
    int8_t* spec_history = (int8_t*)heap_caps_calloc(
        input->bytes, 1, MALLOC_CAP_SPIRAM);
    if (!spec_history) {
        ESP_LOGE(TAG, "Sin PSRAM"); vTaskDelete(nullptr); return;
    }

    // Buffers AFE
    int     feed_size  = s_afe_handle->get_feed_chunksize(s_afe_data);
    int     fetch_size = s_afe_handle->get_fetch_chunksize(s_afe_data);
    int16_t* afe_buf   = (int16_t*)heap_caps_malloc(
        feed_size * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t* pcm_block = (int16_t*)heap_caps_malloc(
        I2S_BLOCK_BYTES, MALLOC_CAP_INTERNAL);
    int afe_written = 0;

    if (!afe_buf || !pcm_block) {
        ESP_LOGE(TAG, "Sin RAM para AFE"); vTaskDelete(nullptr); return;
    }

    SlidingWindow sw;
    sw_reset(&sw);

    bool    in_cooldown  = false;
    int64_t cooldown_end = 0;
    int     log_counter  = 0;
    float   max_avg      = 0.0f;

    ESP_LOGI(TAG, "Escuchando...");

    while (true) {
        if (in_cooldown) {
            int64_t now = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now >= cooldown_end) {
                in_cooldown = false;
                sw_reset(&sw);
                led_set(0, 0, 5);
                ESP_LOGI(TAG, "Escuchando...");
            }
        }

        if (xQueueReceive(s_i2s_queue, pcm_block, portMAX_DELAY) != pdTRUE) continue;

        int rem = I2S_BLOCK_SAMPLES, src = 0;
        while (rem > 0) {
            int space   = feed_size - afe_written;
            int to_copy = (rem < space) ? rem : space;
            memcpy(afe_buf + afe_written, pcm_block + src, to_copy * sizeof(int16_t));
            afe_written += to_copy;
            src         += to_copy;
            rem         -= to_copy;

            if (afe_written < feed_size) break;

            s_afe_handle->feed(s_afe_data, afe_buf);
            afe_written = 0;

            afe_fetch_result_t* result = s_afe_handle->fetch(s_afe_data);
            if (!result || !result->data) continue;

            if (result->vad_state == VAD_SILENCE) {
                sw_reset(&sw);
                continue;
            }

            size_t used;
            struct FrontendOutput fe = FrontendProcessSamples(
                &s_fe_state, result->data, fetch_size, &used);

            if (!fe.values || fe.size != (size_t)NUM_MEL_CHANNELS) continue;

            // Cuantizar frame
            int8_t frame[NUM_MEL_CHANNELS];
            for (int i = 0; i < NUM_MEL_CHANNELS; i++) {
                float   fval = (float)fe.values[i] / 26.0f;
                int32_t qval = (int32_t)roundf(fval / input_scale)
                               + input->params.zero_point;
                if (qval >  127) qval =  127;
                if (qval < -128) qval = -128;
                frame[i] = (int8_t)qval;
            }

            // Sliding window del espectrograma
            memmove(spec_history,
                    spec_history + NUM_MEL_CHANNELS,
                    input->bytes - NUM_MEL_CHANNELS);
            memcpy(spec_history + input->bytes - NUM_MEL_CHANNELS,
                   frame, NUM_MEL_CHANNELS);
            memcpy(input->data.int8, spec_history, input->bytes);

            if (interpreter.Invoke() != kTfLiteOk) continue;

            // Probabilidad
            int8_t raw  = output->data.int8[wake_class];
            float  prob = (float)(raw - out_zp) * out_scale;
            if (prob < 0.0f) prob = 0.0f;
            if (prob > 1.0f) prob = 1.0f;

            float avg = sw_push(&sw, prob);
            if (avg > max_avg) max_avg = avg;

            // Log cada ~30 frames (300ms) para ver más datos
            log_counter++;
            if (log_counter >= 30) {
                ESP_LOGI(TAG, "avg=%.4f | last=%.4f | raw=%d | max=%.4f",
                         avg, prob, (int)raw, max_avg);
                log_counter = 0;
                max_avg     = 0.0f;
            }

            if (!in_cooldown && avg >= PROBABILITY_CUTOFF) {
                ESP_LOGW(TAG, "🟢 ¡HEY ARDO! avg=%.3f", avg);
                led_set(0, 200, 0);
                in_cooldown  = true;
                cooldown_end = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS
                               + COOLDOWN_MS;
                sw_reset(&sw);
            }
        }
    }
}

// =============================================
// APP MAIN
// =============================================
extern "C" void app_main() {
    ESP_LOGI(TAG, "=== Ardo Assistant ===");

    init_led();
    led_set(10, 10, 10);

    tensor_arena = (uint8_t*)heap_caps_malloc(
        TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tensor_arena) {
        tensor_arena = (uint8_t*)heap_caps_malloc(
            TENSOR_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!tensor_arena) { ESP_LOGE(TAG, "SIN MEMORIA"); return; }

    ESP_LOGI(TAG, "RAM interna: %d | PSRAM: %d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    init_microphone();
    init_afe();
    if (!init_frontend()) return;

    s_i2s_queue = xQueueCreate(I2S_QUEUE_DEPTH, I2S_BLOCK_BYTES);
    if (!s_i2s_queue) { ESP_LOGE(TAG, "Cola falló"); return; }

    led_set(0, 0, 15);

    xTaskCreatePinnedToCore(i2s_reader_task, "i2s",
                            4096, nullptr, 6, nullptr, 0);
    xTaskCreatePinnedToCore(wakeword_task,   "wakeword",
                            32768, nullptr, 5, nullptr, 1);

    ESP_LOGI(TAG, "Sistema listo.");
}