/*
 * ============================================================
 * ESP32-C3 SuperMini — Servidor GATT Bidireccional (Aspiradora)
 * Framework : ESP-IDF + NimBLE (C Puro)
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "nvs_flash.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* ─── Configuración de pines ─────────────────────────────────────────────── */
#define LED_PIN         GPIO_NUM_8
#define UART_KL         UART_NUM_1
#define UART_PC         UART_NUM_0   
#define RX_KL           GPIO_NUM_2
#define TX_KL           GPIO_NUM_1
#define UART_BUF        1024

static const char *TAG = "ARDO_GATT";

/* ─── Estado global ───────────────────────────────────────────────────────── */
static volatile bool  g_connected   = false;
static uint16_t       g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t       tx_handle     = 0; 

/* ─── UUIDs del Servicio Nordic UART (NUS) ────────────────────────────────── */
// Servicio: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_svc_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);

// RX (Escritura desde App): 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_chr_rx_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);

// TX (Notificación hacia App): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t gatt_svr_chr_tx_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

/* ─── Declaraciones previas ──────────────────────────────────────────────── */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static void ble_app_advertise(void);

/* ─── LED & UART Helpers ─────────────────────────────────────────────────── */
static void led_set(bool on) { gpio_set_level(LED_PIN, on ? 0 : 1); }
static void led_blink(void)  { led_set(false); vTaskDelay(pdMS_TO_TICKS(30)); led_set(true); }
static void uart_send(uart_port_t port, const char *str) { uart_write_bytes(port, str, strlen(str)); }

/* ─── BLE — Enviar Notificación (Aspiradora -> Celular) ──────────────────── */
static void send_ble_notify(const char *data) {
    if (g_connected && tx_handle != 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, strlen(data));
        ble_gatts_notify_custom(g_conn_handle, tx_handle, om);
        ESP_LOGI(TAG, "[GATT NOTIFY]: %s", data);
    }
}

/* ─── BLE — Callback de Escritura (Celular -> Aspiradora) ────────────────── */
static int gatt_svr_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[128];
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf) - 3) len = sizeof(buf) - 3;
        
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        buf[len] = '\0';
        
        ESP_LOGI(TAG, "[HUB -> ESP32]: %s", buf);
        
        // Reenviar a la KL25Z añadiendo \r\n
        strcat(buf, "\r\n");
        uart_send(UART_KL, buf);
        led_blink();
    }
    return 0;
}

/* ─── BLE — Definición del Árbol GATT ────────────────────────────────────── */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &gatt_svr_chr_rx_uuid.u,
                .access_cb = gatt_svr_chr_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &gatt_svr_chr_tx_uuid.u,
                .access_cb = gatt_svr_chr_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &tx_handle,
            },
            { 0 }
        }
    },
    { 0 }
};

/* ─── BLE — Advertising & Eventos GAP ────────────────────────────────────── */
static void ble_app_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    
    ble_gap_adv_set_fields(&fields);

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb, NULL);
    ESP_LOGI(TAG, "Anunciando por Bluetooth...");
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_connected = true;
            g_conn_handle = event->connect.conn_handle;
            led_set(true);
            ESP_LOGI(TAG, "*** HUB CONECTADO ***");
        } else {
            ble_app_advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        g_connected = false;
        led_set(false);
        ESP_LOGI(TAG, "*** HUB DESCONECTADO ***");
        ble_app_advertise();
        break;
    }
    return 0;
}

static void ble_app_on_sync(void) {
    ble_app_advertise();
}

/* ─── Procesamiento de Comandos KL25Z / PC ───────────────────────────────── */
static void process_command(const char *raw, bool from_kl) {
    char cmd[128];
    strncpy(cmd, raw, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    
    char *p = cmd;
    while(isspace((unsigned char)*p)) p++;
    char *end = p + strlen(p) - 1;
    while(end > p && isspace((unsigned char)*end)) *end-- = '\0';
    for (char *c = p; *c; c++) *c = toupper((unsigned char)*c);

    if (strlen(p) == 0) return;

    if (from_kl) {
        led_blink();
        ESP_LOGI(TAG, "[KL25Z]: %s", p);
    } else {
        ESP_LOGI(TAG, "[PC]: %s", p);
    }

    char resp[64] = "ERR+CMD";
    bool interceptado = false;

    if (strncmp(p, "AT+BATT:", 8) == 0) {
        char notif[32];
        snprintf(notif, sizeof(notif), "BATERIA:%s%%", p + 8);
        if (g_connected) send_ble_notify(notif);
        strcpy(resp, "OK+NOTIFIED");
        interceptado = true;
    }
    else if (strncmp(p, "AT+ALERT:", 9) == 0) {
        char notif[64];
        snprintf(notif, sizeof(notif), "ALERTA:%s", p + 9);
        if (g_connected) send_ble_notify(notif);
        strcpy(resp, "OK+NOTIFIED");
        interceptado = true;
    }
    else if (strcmp(p, "AT") == 0) {
        strcpy(resp, "OK");
        interceptado = true;
    }
    else if (strcmp(p, "AT+INQ") == 0) {
        strcpy(resp, "OK+INQ_DONE");
        interceptado = true;
    }
    else if (strcmp(p, "AT+RSSI") == 0) {
        // --- AQUÍ ESTÁ LA LECTURA REAL DEL RSSI ---
        if (g_connected && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            int8_t rssi = 0;
            // Pedimos el RSSI actual a la controladora Bluetooth
            int rc = ble_gap_conn_rssi(g_conn_handle, &rssi);
            if (rc == 0) {
                snprintf(resp, sizeof(resp), "OK+RSSI:%d", rssi);
            } else {
                strcpy(resp, "ERR+RSSI_READ");
            }
        } else {
            // Si no hay teléfono conectado, no hay señal que medir
            strcpy(resp, "OK+RSSI:-100"); 
        }
        interceptado = true;
    }

    if (!interceptado) {
        if (g_connected) {
            send_ble_notify(p);
            strcpy(resp, "OK+SENT");
        } else {
            strcpy(resp, "ERR+DISCONN");
        }
    }

    char line[128];
    snprintf(line, sizeof(line), "%s\r\n", resp);
    
    if (from_kl) {
        uart_send(UART_KL, line);
        ESP_LOGI(TAG, "[UART TX -> KL25Z]: %s", resp);
    } else {
        uart_send(UART_PC, line);
        ESP_LOGI(TAG, "[UART TX -> PC]: %s", resp);
    }
}

/* ─── Tareas de UART ─────────────────────────────────────────────────────── */
static void uart_kl_task(void *arg) {
    uint8_t byte;
    char buf[128];
    int idx = 0;
    for (;;) {
        if (uart_read_bytes(UART_KL, &byte, 1, portMAX_DELAY) > 0) {
            if (byte == '\n' || byte == '\r') {
                buf[idx] = '\0';
                if (idx > 0) process_command(buf, true);
                idx = 0;
            } else if (idx < sizeof(buf) - 1) {
                buf[idx++] = (char)byte;
            }
        }
    }
}

static void uart_pc_task(void *arg) {
    uint8_t byte;
    char buf[128];
    int idx = 0;
    for (;;) {
        if (uart_read_bytes(UART_PC, &byte, 1, portMAX_DELAY) > 0) {
            if (byte == '\n' || byte == '\r') {
                buf[idx] = '\0';
                if (idx > 0) process_command(buf, false);
                idx = 0;
            } else if (idx < sizeof(buf) - 1) {
                buf[idx++] = (char)byte;
            }
        }
    }
}

/* ─── Tarea NimBLE ───────────────────────────────────────────────────────── */
void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ─── Main ────────────────────────────────────────────────────────────────── */
void app_main(void) {
    /* 1. Inicializar NVS (Requerido por BLE) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Configurar Pines */
    gpio_config_t led_cfg = { .pin_bit_mask = (1ULL << LED_PIN), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&led_cfg);
    led_set(false); 

    /* 3. Configurar UARTs */
    uart_config_t uart_kl_cfg = {
        .baud_rate = 9600, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_KL, UART_BUF, UART_BUF, 0, NULL, 0);
    uart_param_config(UART_KL, &uart_kl_cfg);
    uart_set_pin(UART_KL, TX_KL, RX_KL, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    
    // UART PC (ya iniciado por IDF, solo reinstalamos driver para leer fácil)
    uart_driver_install(UART_PC, UART_BUF, UART_BUF, 0, NULL, 0);

    /* 4. Inicializar NimBLE */
    nimble_port_init();
    
    ble_svc_gap_device_name_set("Aspiradora_Ardo");
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);
    ble_hs_cfg.sync_cb = ble_app_on_sync;

    nimble_port_freertos_init(nimble_host_task);

    /* 5. Tareas del UART con memoria suficiente */
    xTaskCreate(uart_kl_task, "uart_kl", 4096, NULL, 5, NULL);
    xTaskCreate(uart_pc_task, "uart_pc", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Cerebro BLE iniciado (C Nativo). Servidor GATT activo.");
}