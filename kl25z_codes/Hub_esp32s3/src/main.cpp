/*
 * ============================================================
 * ESP32-S3 SuperMini — HUB Central
 * ============================================================
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_netif.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

/* ─── CREDENCIALES ───────────────────────────────────────────────────────── */
#define WIFI_SSID    "Ardo"
#define WIFI_PASS    "ardi2siempre"

#define MQTT_URI  "mqtt://TU_IP_BROKER"
#define MQTT_USER "mqtt_ardo"
#define MQTT_PASS "admin"

#define TOPIC_BATT   "homeassistant/sensor/aspiradora_bateria/state"
#define TOPIC_ALERT  "homeassistant/sensor/aspiradora_alerta/state"
#define TOPIC_STATUS "homeassistant/sensor/aspiradora_conexion/state"

static const char *TAG = "HUB_ARDO";

/* ─── Estado global ──────────────────────────────────────────────────────── */
static EventGroupHandle_t       s_eg;
#define BIT_IP_UP BIT0

static esp_mqtt_client_handle_t mqtt_client    = NULL;
static volatile bool            ble_is_synced  = false;
static volatile bool            mqtt_connected = false;
static volatile bool            is_scanning    = false;
static volatile bool            wifi_up        = false;
static volatile bool            s_mqtt_started = false;
static uint16_t                 g_conn_handle  = BLE_HS_CONN_HANDLE_NONE;
static TimerHandle_t            s_reconnect_timer = NULL;
static TaskHandle_t             s_wifi_task    = NULL;
static volatile int             s_consec_fails = 0;
static uint32_t                 s_retry_delay  = 1000;

static const char *DEVICE_NAME = "Aspiradora_Ardo";

static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E);

/* ─── Forward declarations ───────────────────────────────────────────────── */
static void ble_app_scan(void);
static void ble_app_on_sync(void);
void        nimble_host_task(void *param);

/* =========================================================================
 * WI-FI
 * ========================================================================= */
static void wifi_do_connect(void) {
    esp_wifi_connect();
    ESP_LOGI(TAG, "Intentando conectar...");
}

static void reconnect_timer_cb(TimerHandle_t xTimer) {
    if (s_wifi_task) xTaskNotifyGive(s_wifi_task);
}

static void wifi_reconnect_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_consec_fails++;

        if (s_consec_fails >= 5) {
            s_consec_fails = 0;
            s_retry_delay  = 1000;
            ESP_LOGW(TAG, "5 fallos consecutivos — hard reset radio WiFi...");
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_start();
            continue;
        }

        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
        wifi_do_connect();
    }
}

static void schedule_reconnect(uint32_t delay_ms) {
    if (s_reconnect_timer == NULL) {
        s_reconnect_timer = xTimerCreate("wifi_reconnect",
                                         pdMS_TO_TICKS(delay_ms),
                                         pdFALSE, NULL,
                                         reconnect_timer_cb);
    } else {
        xTimerChangePeriod(s_reconnect_timer,
                           pdMS_TO_TICKS(delay_ms), 0);
    }
    xTimerStart(s_reconnect_timer, 0);
    ESP_LOGI(TAG, "Reconexión en %lums...", (unsigned long)delay_ms);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi start.");
        wifi_do_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        wifi_up = false;
        xEventGroupClearBits(s_eg, BIT_IP_UP);
        ESP_LOGW(TAG, "Desconectado. Razón: %d | próximo intento en %lums",
                 d->reason, (unsigned long)s_retry_delay);

        schedule_reconnect(s_retry_delay);
        s_retry_delay = (s_retry_delay * 2 > 10000) ? 10000 : s_retry_delay * 2;

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_up = true;
        s_retry_delay  = 1000;
        s_consec_fails = 0;
        xEventGroupSetBits(s_eg, BIT_IP_UP);
        if (!s_mqtt_started) {
            esp_mqtt_client_start(mqtt_client);
            s_mqtt_started = true;
        }
        if (ble_is_synced && !is_scanning) ble_app_scan();
    }
}

/* ─── MQTT ───────────────────────────────────────────────────────────────── */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    if (ev->event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT conectado.");
        mqtt_connected = true;
        esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS,
                                "Buscando Aspiradora...", 0, 1, 1);
        if (ble_is_synced && !is_scanning) ble_app_scan();
    } else if (ev->event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "MQTT desconectado.");
        mqtt_connected = false;
    } else if (ev->event_id == MQTT_EVENT_ERROR) {
        ESP_LOGE(TAG, "Error MQTT.");
    }
}

/* ─── BLE ────────────────────────────────────────────────────────────────── */
static void parse_and_publish(const char *buf) {
    if (strncmp(buf, "BATERIA:", 8) == 0) {
        char val[16];
        strncpy(val, buf + 8, sizeof(val) - 1);
        val[sizeof(val) - 1] = '\0';
        char *pct = strchr(val, '%');
        if (pct) *pct = '\0';
        ESP_LOGI(TAG, "Batería: %s%%", val);
        esp_mqtt_client_publish(mqtt_client, TOPIC_BATT, val, 0, 1, 0);
    } else if (strncmp(buf, "ALERTA:", 7) == 0) {
        ESP_LOGI(TAG, "Alerta: %s", buf + 7);
        esp_mqtt_client_publish(mqtt_client, TOPIC_ALERT, buf + 7, 0, 1, 0);
    } else {
        ESP_LOGI(TAG, "MSG: %s", buf);
    }
}

static int ble_on_disc_char(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0 &&
        ble_uuid_cmp(&chr->uuid.u, &tx_uuid.u) == 0) {
        ESP_LOGI(TAG, "TX encontrada. Suscribiendo...");
        uint8_t cccd[] = {0x01, 0x00};
        ble_gattc_write_flat(conn_handle, chr->val_handle + 1,
                             cccd, sizeof(cccd), NULL, NULL);
        esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS,
                                "Aspiradora Conectada", 0, 1, 1);
    }
    return 0;
}

static int ble_on_disc_svc(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg) {
    if (error->status == 0 &&
        ble_uuid_cmp(&svc->uuid.u, &svc_uuid.u) == 0) {
        ESP_LOGI(TAG, "Servicio NUS. Buscando características...");
        ble_gattc_disc_all_chrs(conn_handle, svc->start_handle,
                                svc->end_handle, ble_on_disc_char, NULL);
    }
    return 0;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data);
        if (fields.name_len > 0) {
            char name[32];
            int len = fields.name_len > 31 ? 31 : (int)fields.name_len;
            memcpy(name, fields.name, len);
            name[len] = '\0';
            if (strcmp(name, DEVICE_NAME) == 0) {
                ESP_LOGI(TAG, "Aspiradora encontrada. Conectando BLE...");
                ble_gap_disc_cancel();
                is_scanning = false;
                ble_gap_connect(BLE_OWN_ADDR_PUBLIC,
                                &event->disc.addr, 4000,
                                NULL, ble_gap_event, NULL);
            }
        }
        break;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            is_scanning = false;
            ESP_LOGI(TAG, "BLE conectado. Descubriendo servicios...");
            ble_gattc_disc_all_svcs(g_conn_handle, ble_on_disc_svc, NULL);
        } else {
            is_scanning = false;
            if (mqtt_connected && wifi_up) ble_app_scan();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        is_scanning = false;
        ESP_LOGW(TAG, "BLE desconectado.");
        esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS,
                                "Aspiradora Desconectada", 0, 1, 1);
        if (mqtt_connected && wifi_up) ble_app_scan();
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        char buf[128];
        int len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > 127) len = 127;
        os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
        buf[len] = '\0';
        parse_and_publish(buf);
        break;
    }
    }
    return 0;
}

static void ble_app_scan(void) {
    if (is_scanning || !wifi_up) return;
    struct ble_gap_disc_params p = {};
    p.itvl    = BLE_GAP_SCAN_ITVL_MS(200);
    p.window  = BLE_GAP_SCAN_WIN_MS(50);
    p.passive = 1;
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                 &p, ble_gap_event, NULL);
    is_scanning = true;
    ESP_LOGI(TAG, "BLE: Escaneando...");
}

static void ble_app_on_sync(void) {
    ESP_LOGI(TAG, "NimBLE sincronizado.");
    ble_is_synced = true;
    if (mqtt_connected && wifi_up && !is_scanning) ble_app_scan();
}

void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* =========================================================================
 * MAIN
 * ========================================================================= */
extern "C" void app_main(void) {

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupta, borrando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_ret);
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_eg = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_t sta_cfg = {};
    memcpy(sta_cfg.sta.ssid,     WIFI_SSID, strlen(WIFI_SSID));
    memcpy(sta_cfg.sta.password, WIFI_PASS, strlen(WIFI_PASS));
    sta_cfg.sta.scan_method        = WIFI_FAST_SCAN;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    sta_cfg.sta.threshold.rssi     = -127;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    esp_mqtt_client_config_t mcfg = {};
    mcfg.broker.address.uri                  = MQTT_URI;
    mcfg.credentials.username                = MQTT_USER;
    mcfg.credentials.authentication.password = MQTT_PASS;
    mqtt_client = esp_mqtt_client_init(&mcfg);
    esp_mqtt_client_register_event(mqtt_client,
        (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
        mqtt_event_handler, NULL);

    xTaskCreatePinnedToCore(wifi_reconnect_task, "wifi_reconnect",
                            4096, NULL, 5, &s_wifi_task, 0);

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Esperando IP...");
    xEventGroupWaitBits(s_eg, BIT_IP_UP,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "IP obtenida. Iniciando BLE...");
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "Hub listo.");
}
