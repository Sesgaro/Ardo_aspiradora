/*
 * ============================================================
 * ESP32-S3 SuperMini — HUB Central
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
#include "esp_coexist.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

/* ─── CREDENCIALES ───────────────────────────────────────────────────────── */
#define WIFI_SSID    "Ardo"
#define WIFI_PASS    "ardi2siempre"

#define MQTT_URI  "mqtt://10.42.0.1"
#define MQTT_USER "mqtt_ardo"
#define MQTT_PASS "admin"

/* ─── TOPICS ─────────────────────────────────────────────────────────────── */
#define TOPIC_STATUS      "homeassistant/sensor/aspiradora_conexion/state"
#define TOPIC_ALERT       "homeassistant/sensor/aspiradora_alerta/state"
#define TOPIC_VAC_STATE   "homeassistant/vacuum/aspiradora/state"
#define TOPIC_VAC_CMD     "homeassistant/vacuum/aspiradora/command"
#define TOPIC_DISCOVERY   "homeassistant/vacuum/aspiradora/config"

static const char *TAG = "HUB_ARDO";

/* ─── Estado global ──────────────────────────────────────────────────────── */
static EventGroupHandle_t       s_eg;
#define BIT_IP_UP BIT0

static esp_mqtt_client_handle_t mqtt_client      = NULL;
static volatile bool            ble_is_synced    = false;
static volatile bool            mqtt_connected   = false;
static volatile bool            is_scanning      = false;
static volatile bool            wifi_up          = false;
static volatile bool            s_mqtt_started   = false;
static uint16_t                 g_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
static uint16_t                 g_rx_char_handle = 0;
static TimerHandle_t            s_reconnect_timer  = NULL;
static TimerHandle_t            s_ble_retry_timer  = NULL;
static TaskHandle_t             s_wifi_task      = NULL;
static volatile int             s_consec_fails   = 0;
static uint32_t                 s_retry_delay    = 1000;
static int32_t                  g_battery_level  = -1;
static char                     g_vac_state[16]  = "docked";

static const char *DEVICE_NAME = "Aspiradora_Ardo";

/* NUS service / características */
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E);
/* TX (aspiradora → hub): notificaciones */
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E);
/* RX (hub → aspiradora): escritura de comandos */
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x6E);

/* ─── Forward declarations ───────────────────────────────────────────────── */
static void ble_app_scan(void);
static void ble_app_on_sync(void);
void        nimble_host_task(void *param);

/* =========================================================================
 * HELPERS: MQTT STATE + DISCOVERY
 * ========================================================================= */

static void publish_vac_state(void) {
    char buf[64];
    if (g_battery_level >= 0) {
        snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"battery_level\":%ld}",
                 g_vac_state, (long)g_battery_level);
    } else {
        snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}", g_vac_state);
    }
    /* retain=1: HA recuerda el último estado tras reinicio */
    esp_mqtt_client_publish(mqtt_client, TOPIC_VAC_STATE, buf, 0, 1, 1);
}

/* Registra la entidad vacuum en Home Assistant via MQTT Discovery */
static void publish_discovery(void) {
    const char *payload =
        "{"
          "\"name\":\"Aspiradora Ardo\","
          "\"unique_id\":\"aspiradora_ardo_hub\","
          "\"schema\":\"state\","
          "\"command_topic\":\"homeassistant/vacuum/aspiradora/command\","
          "\"state_topic\":\"homeassistant/vacuum/aspiradora/state\","
          "\"availability_topic\":\"homeassistant/sensor/aspiradora_conexion/state\","
          "\"payload_available\":\"Aspiradora Conectada\","
          "\"payload_not_available\":\"Aspiradora Desconectada\","
          "\"device\":{"
            "\"identifiers\":[\"aspiradora_ardo\"],"
            "\"name\":\"Aspiradora Ardo\","
            "\"manufacturer\":\"Ardo\""
          "}"
        "}";
    esp_mqtt_client_publish(mqtt_client, TOPIC_DISCOVERY, payload, 0, 1, 1);
    /* Disponibilidad inicial: hub online, esperando conexión BLE */
    esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS, "Aspiradora Desconectada", 0, 1, 1);
    /* Estado inicial retenido para que HA muestre algo inmediatamente */
    esp_mqtt_client_publish(mqtt_client, TOPIC_VAC_STATE, "{\"state\":\"docked\"}", 0, 1, 1);
    ESP_LOGI(TAG, "MQTT Discovery publicado.");
}

/* =========================================================================
 * BLE WRITE — envío de comandos a la aspiradora
 * ========================================================================= */

static void ble_write_cmd(const char *cmd) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_rx_char_handle == 0) {
        ESP_LOGW(TAG, "Aspiradora no conectada, ignorando cmd: %s", cmd);
        return;
    }
    /* Verificar que la conexión sigue viva antes de escribir */
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(g_conn_handle, &desc) != 0) {
        ESP_LOGW(TAG, "Conexión BLE caída, descartando cmd: %s", cmd);
        g_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
        g_rx_char_handle = 0;
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(cmd, strlen(cmd));
    if (!om) { ESP_LOGE(TAG, "ble_write_cmd: mbuf alloc fail"); return; }
    int rc = ble_gattc_write_no_rsp(g_conn_handle, g_rx_char_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_write_cmd error: %d — reseteando handle", rc);
        g_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
        g_rx_char_handle = 0;
    } else {
        ESP_LOGI(TAG, "CMD → Aspiradora: %s", cmd);
    }
}

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
        s_consec_fails = s_consec_fails + 1;

        /* Tras varios ciclos completos fallidos, resetear la radio por si el
         * driver/controlador quedó en mal estado. El umbral es alto porque el
         * driver ya reintenta internamente (failure_retry_cnt). */
        if (s_consec_fails >= 8) {
            s_consec_fails = 0;
            s_retry_delay  = 1000;
            ESP_LOGW(TAG, "8 fallos consecutivos — hard reset radio WiFi...");
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_start();   /* dispara WIFI_EVENT_STA_START → connect */
            continue;
        }

        /* Ya estamos desconectados: reconectar directo, sin disconnect previo
         * (evita la carrera que tumbaba el handshake a medio camino). */
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
        wifi_up        = true;
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

/* =========================================================================
 * MQTT
 * ========================================================================= */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch (ev->event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado.");
        mqtt_connected = true;
        publish_discovery();
        esp_mqtt_client_subscribe(mqtt_client, TOPIC_VAC_CMD, 1);
        esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS,
                                "Buscando Aspiradora...", 0, 1, 1);
        if (ble_is_synced && !is_scanning) ble_app_scan();
        break;

    case MQTT_EVENT_DATA: {
        /* Comandos de HA → aspiradora vía BLE */
        char cmd[32] = {0};
        int  len = ev->data_len < 31 ? ev->data_len : 31;
        memcpy(cmd, ev->data, len);
        ESP_LOGI(TAG, "MQTT cmd: [%s]", cmd);

        if (strcmp(cmd, "start") == 0) {
            ble_write_cmd("CMD:INICIAR");
            strncpy(g_vac_state, "cleaning",  sizeof(g_vac_state) - 1);
            publish_vac_state();
        } else if (strcmp(cmd, "pause") == 0) {
            ble_write_cmd("CMD:PAUSA");
            strncpy(g_vac_state, "paused",    sizeof(g_vac_state) - 1);
            publish_vac_state();
        } else if (strcmp(cmd, "stop") == 0) {
            ble_write_cmd("CMD:DETENER");
            strncpy(g_vac_state, "idle",      sizeof(g_vac_state) - 1);
            publish_vac_state();
        } else if (strcmp(cmd, "return_to_base") == 0) {
            ble_write_cmd("CMD:VOLVER");
            strncpy(g_vac_state, "returning", sizeof(g_vac_state) - 1);
            publish_vac_state();
        } else if (strcmp(cmd, "locate") == 0) {
            ble_write_cmd("CMD:LOCALIZAR");
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT desconectado.");
        mqtt_connected = false;
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Error MQTT.");
        break;

    default:
        break;
    }
}

/* =========================================================================
 * BLE
 * ========================================================================= */

/* Mensajes de la aspiradora → actualiza estado en HA */
static void parse_and_publish(const char *buf) {
    if (strncmp(buf, "BATERIA:", 8) == 0) {
        char val[8];
        strncpy(val, buf + 8, sizeof(val) - 1);
        val[sizeof(val) - 1] = '\0';
        char *pct = strchr(val, '%');
        if (pct) *pct = '\0';
        g_battery_level = atoi(val);
        ESP_LOGI(TAG, "Batería: %ld%%", (long)g_battery_level);
        publish_vac_state();

    } else if (strncmp(buf, "ESTADO:", 7) == 0) {
        const char *est = buf + 7;
        if      (strcmp(est, "LIMPIANDO") == 0) strncpy(g_vac_state, "cleaning",  sizeof(g_vac_state) - 1);
        else if (strcmp(est, "PAUSADO")   == 0) strncpy(g_vac_state, "paused",    sizeof(g_vac_state) - 1);
        else if (strcmp(est, "VOLVIENDO") == 0) strncpy(g_vac_state, "returning", sizeof(g_vac_state) - 1);
        else if (strcmp(est, "EN_BASE")   == 0) strncpy(g_vac_state, "docked",    sizeof(g_vac_state) - 1);
        else if (strcmp(est, "DETENIDO")  == 0) strncpy(g_vac_state, "idle",      sizeof(g_vac_state) - 1);
        else if (strcmp(est, "ERROR")     == 0) strncpy(g_vac_state, "error",     sizeof(g_vac_state) - 1);
        ESP_LOGI(TAG, "Estado aspiradora: %s", g_vac_state);
        publish_vac_state();

    } else if (strncmp(buf, "ALERTA:", 7) == 0) {
        ESP_LOGI(TAG, "Alerta: %s", buf + 7);
        esp_mqtt_client_publish(mqtt_client, TOPIC_ALERT, buf + 7, 0, 1, 0);

    } else {
        ESP_LOGI(TAG, "MSG: %s", buf);
    }
}

/* Callback del write al CCCD: confirma si la suscripción a notificaciones tuvo éxito */
static int ble_on_subscribe(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, ">>> SUSCRITO a notificaciones TX (handle=%d). OK <<<",
                 attr ? attr->handle : 0);
        esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS,
                                "Aspiradora Conectada", 0, 1, 1);
    } else {
        ESP_LOGE(TAG, ">>> ERROR al suscribir CCCD: status=%d <<<", error->status);
    }
    return 0;
}

static int ble_on_disc_char(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg) {
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "disc_char error: %d", error->status);
        return 0;
    }
    if (chr == NULL) return 0;

    if (ble_uuid_cmp(&chr->uuid.u, &tx_uuid.u) == 0) {
        /* CCCD del NUS está en val_handle + 1 */
        uint16_t cccd_handle = chr->val_handle + 1;
        ESP_LOGI(TAG, "TX encontrada (val=%d). Suscribiendo CCCD en handle=%d...",
                 chr->val_handle, cccd_handle);
        uint8_t cccd[] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(conn_handle, cccd_handle,
                                      cccd, sizeof(cccd), ble_on_subscribe, NULL);
        if (rc != 0) ESP_LOGE(TAG, "ble_gattc_write_flat (CCCD) error: %d", rc);
    } else if (ble_uuid_cmp(&chr->uuid.u, &rx_uuid.u) == 0) {
        g_rx_char_handle = chr->val_handle;
        ESP_LOGI(TAG, "RX encontrada (handle=%d). Listo para enviar comandos.", g_rx_char_handle);
    }
    return 0;
}

static int ble_on_disc_svc(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg) {
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "disc_svc error: %d", error->status);
        return 0;
    }
    if (svc == NULL) return 0;

    if (ble_uuid_cmp(&svc->uuid.u, &svc_uuid.u) == 0) {
        ESP_LOGI(TAG, "Servicio NUS encontrado (handles %d-%d). Buscando características...",
                 svc->start_handle, svc->end_handle);
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
            g_conn_handle    = event->connect.conn_handle;
            g_rx_char_handle = 0;
            is_scanning      = false;
            strncpy(g_vac_state, "idle", sizeof(g_vac_state) - 1);
            publish_vac_state();
            ESP_LOGI(TAG, "BLE conectado (handle=%d). Descubriendo servicios...",
                     g_conn_handle);
            ble_gattc_disc_all_svcs(g_conn_handle, ble_on_disc_svc, NULL);
        } else {
            ESP_LOGW(TAG, "Conexión BLE falló (status=%d). Reintentando scan...",
                     event->connect.status);
            is_scanning = false;
            if (mqtt_connected && wifi_up) ble_app_scan();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
        g_rx_char_handle = 0;
        g_battery_level  = -1;
        is_scanning      = false;
        strncpy(g_vac_state, "docked", sizeof(g_vac_state) - 1);
        ESP_LOGW(TAG, "BLE desconectado.");
        esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS,
                                "Aspiradora Desconectada", 0, 1, 1);
        if (mqtt_connected && wifi_up) ble_app_scan();
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* Solo ocurre porque cancelamos el scan para conectar (usamos
         * BLE_HS_FOREVER). NO relanzar scan aquí: el evento CONNECT decide
         * — si conecta se queda, si falla vuelve a escanear. Relanzar aquí
         * dejaría la radio escaneando mientras conecta y mataría las
         * notificaciones. */
        is_scanning = false;
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        char buf[128];
        int len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > 127) len = 127;
        os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
        buf[len] = '\0';
        ESP_LOGI(TAG, "<<< NOTIFY RX (attr=%d): %s >>>",
                 event->notify_rx.attr_handle, buf);
        parse_and_publish(buf);
        break;
    }

    default:
        break;
    }
    return 0;
}

static void ble_scan_retry_cb(TimerHandle_t xTimer) {
    ble_app_scan();
}

static void ble_app_scan(void) {
    /* No escanear si ya hay conexión activa (escanear+conectado en ESP32
     * interfiere con la recepción de notificaciones) ni si ya escaneamos. */
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;
    if (is_scanning || !wifi_up) return;
    struct ble_gap_disc_params p = {};
    p.itvl    = BLE_GAP_SCAN_ITVL_MS(200);
    p.window  = BLE_GAP_SCAN_WIN_MS(50);
    p.passive = 1;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                          &p, ble_gap_event, NULL);
    if (rc == 0) {
        is_scanning = true;
        ESP_LOGI(TAG, "BLE: Escaneando...");
    } else {
        ESP_LOGW(TAG, "BLE: scan error %d, reintentando en 2s...", rc);
        if (s_ble_retry_timer == NULL)
            s_ble_retry_timer = xTimerCreate("ble_retry", pdMS_TO_TICKS(2000),
                                             pdFALSE, NULL, ble_scan_retry_cb);
        else
            xTimerChangePeriod(s_ble_retry_timer, pdMS_TO_TICKS(2000), 0);
        xTimerStart(s_ble_retry_timer, 0);
    }
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
    /* MIN_MODEM (no NONE): en coexistencia WiFi+BLE, el WiFi debe dormir entre
     * beacons para cederle slots de radio al BLE. Con PS_NONE el WiFi acapara
     * la radio y el hub pierde TODAS las notificaciones BLE (el C3 las envía
     * con rc=0 pero nunca llegan). MIN_MODEM resuelve la recepción de notifys. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    /* Priorizar BLE en el árbitro de coexistencia: el hub recibe respuestas a
     * sus requests (el radio se enfoca en BLE un instante) pero perdía las
     * notificaciones espontáneas porque el WiFi ganaba la radio en esos
     * connection events. PREFER_BT le da a BLE prioridad de RF. */
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    wifi_config_t sta_cfg = {};
    memcpy(sta_cfg.sta.ssid,     WIFI_SSID, strlen(WIFI_SSID));
    memcpy(sta_cfg.sta.password, WIFI_PASS, strlen(WIFI_PASS));
    /* Canal fijo 6: el router está fijado a ese canal. Escaneo rápido solo en
     * ese canal → conexión más veloz y sin confundirse con otros APs. */
    sta_cfg.sta.channel            = 6;
    sta_cfg.sta.scan_method        = WIFI_FAST_SCAN;
    sta_cfg.sta.sort_method        = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.threshold.rssi     = -127;
    sta_cfg.sta.pmf_cfg.capable    = true;
    sta_cfg.sta.pmf_cfg.required   = false;
    /* Reintentos a nivel del driver: ante un handshake fallido (razón 4/15/205)
     * el supplicant reintenta solo, sin tumbar la conexión ni esperar a nuestro
     * backoff. Esto resuelve los fallos transitorios de asociación. */
    sta_cfg.sta.failure_retry_cnt  = 5;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    /* Forzar protocolo 802.11 b/g/n explícito: algunos routers (Asus incluido)
     * fallan la asociación si el cliente negocia solo HT/HE. */
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

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
