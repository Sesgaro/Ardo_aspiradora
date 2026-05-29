/*
 * ============================================================
 * ESP32-C3 SuperMini — Servidor GATT Bidireccional (Aspiradora)
 * Soporta hasta 2 clientes BLE simultáneos:
 *   - Hub ESP32-S3 (comandos + notificaciones)
 *   - Celular      (waypoints + notificaciones)
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

#define MAX_CONNS       2

static const char *TAG = "ARDO_GATT";

/* ─── Estado global ───────────────────────────────────────────────────────── */
static uint16_t g_conn_handles[MAX_CONNS];
static int      g_conn_count = 0;
static uint16_t tx_handle    = 0;

/* ─── UUIDs del Servicio Nordic UART (NUS) ────────────────────────────────── */
static const ble_uuid128_t gatt_svr_svc_uuid =
    BLE_UUID128_INIT(0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
                     0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E);

static const ble_uuid128_t gatt_svr_chr_rx_uuid =
    BLE_UUID128_INIT(0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
                     0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x6E);

static const ble_uuid128_t gatt_svr_chr_tx_uuid =
    BLE_UUID128_INIT(0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
                     0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E);

/* ─── Forward declarations ───────────────────────────────────────────────── */
static int  ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static void ble_app_advertise(void);

/* ─── LED & UART Helpers ─────────────────────────────────────────────────── */
static void led_set(bool on)  { gpio_set_level(LED_PIN, on ? 0 : 1); }
static void led_blink(void)   { led_set(false); vTaskDelay(pdMS_TO_TICKS(30)); led_set(true); }
static void uart_send(uart_port_t port, const char *str) { uart_write_bytes(port, str, strlen(str)); }

/* ─── Gestión de conexiones múltiples ────────────────────────────────────── */
static void conn_add(uint16_t handle) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conn_handles[i] == BLE_HS_CONN_HANDLE_NONE) {
            g_conn_handles[i] = handle;
            g_conn_count++;
            return;
        }
    }
}

static void conn_remove(uint16_t handle) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conn_handles[i] == handle) {
            g_conn_handles[i] = BLE_HS_CONN_HANDLE_NONE;
            g_conn_count--;
            return;
        }
    }
}

/* ─── BLE — Enviar Notificación a TODOS los clientes conectados ──────────── */
static void send_ble_notify(const char *data) {
    if (tx_handle == 0) {
        ESP_LOGW(TAG, "[NOTIFY] tx_handle=0, sin caracteristica TX");
        return;
    }
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conn_handles[i] == BLE_HS_CONN_HANDLE_NONE) continue;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, strlen(data));
        if (!om) { ESP_LOGE(TAG, "[NOTIFY] mbuf alloc fail"); continue; }
        int rc = ble_gatts_notify_custom(g_conn_handles[i], tx_handle, om);
        ESP_LOGI(TAG, "[NOTIFY → conn=%d, tx_handle=%d, rc=%d]: %s",
                 g_conn_handles[i], tx_handle, rc, data);
    }
}

/* ─── BLE — Callback de Escritura (cualquier cliente → KL25Z) ────────────── */
static int gatt_svr_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[128];
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > (int)sizeof(buf) - 3) len = sizeof(buf) - 3;
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        buf[len] = '\0';
        ESP_LOGI(TAG, "[CLIENTE %d → KL25Z]: %s", conn_handle, buf);
        strcat(buf, "\r\n");
        uart_send(UART_KL, buf);
        led_blink();
    }
    return 0;
}

/* ─── BLE — Árbol GATT ───────────────────────────────────────────────────── */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &gatt_svr_chr_rx_uuid.u,
                .access_cb = gatt_svr_chr_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &gatt_svr_chr_tx_uuid.u,
                .access_cb  = gatt_svr_chr_access_cb,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &tx_handle,
            },
            { 0 }
        }
    },
    { 0 }
};

/* ─── BLE — Advertising & Eventos GAP ────────────────────────────────────── */
static void ble_app_advertise(void) {
    /* No anunciar si ya tenemos el máximo de conexiones */
    if (g_conn_count >= MAX_CONNS) return;

    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields  fields;

    memset(&fields, 0, sizeof(fields));
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name             = ble_svc_gap_device_name();
    fields.name                  = (uint8_t *)name;
    fields.name_len              = strlen(name);
    fields.name_is_complete      = 1;
    ble_gap_adv_set_fields(&fields);

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, ble_gap_event_cb, NULL);
    ESP_LOGI(TAG, "Anunciando... (%d/%d conexiones activas)", g_conn_count, MAX_CONNS);
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (g_conn_count < MAX_CONNS) {
                conn_add(event->connect.conn_handle);
                ESP_LOGI(TAG, "*** CLIENTE CONECTADO (handle=%d, total=%d) ***",
                         event->connect.conn_handle, g_conn_count);
                if (g_conn_count == 1) led_set(true);
            } else {
                /* No hay lugar — rechazar */
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
                ESP_LOGW(TAG, "Conexión rechazada: límite alcanzado");
            }
        }
        /* Seguir anunciando para el segundo cliente */
        ble_app_advertise();
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        conn_remove(event->disconnect.conn.conn_handle);
        ESP_LOGI(TAG, "*** CLIENTE DESCONECTADO (handle=%d, total=%d, razon=%d) ***",
                 event->disconnect.conn.conn_handle, g_conn_count,
                 event->disconnect.reason);
        if (g_conn_count == 0) led_set(false);
        ble_app_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "*** SUBSCRIBE (conn=%d, attr=%d, notify=%d) ***",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
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
    while (isspace((unsigned char)*p)) p++;
    char *end = p + strlen(p) - 1;
    while (end > p && isspace((unsigned char)*end)) *end-- = '\0';
    for (char *c = p; *c; c++) *c = toupper((unsigned char)*c);

    if (strlen(p) == 0) return;

    if (from_kl) {
        led_blink();
        ESP_LOGI(TAG, "[KL25Z]: %s", p);
    } else {
        ESP_LOGI(TAG, "[PC]: %s", p);
    }

    char resp[64]    = "ERR+CMD";
    bool interceptado = false;

    if (strncmp(p, "AT+BATT:", 8) == 0) {
        char notif[32];
        snprintf(notif, sizeof(notif), "BATERIA:%s%%", p + 8);
        send_ble_notify(notif);
        strcpy(resp, "OK+NOTIFIED");
        interceptado = true;

    } else if (strncmp(p, "AT+ALERT:", 9) == 0) {
        char notif[64];
        snprintf(notif, sizeof(notif), "ALERTA:%s", p + 9);
        send_ble_notify(notif);
        strcpy(resp, "OK+NOTIFIED");
        interceptado = true;

    } else if (strncmp(p, "AT+ESTADO:", 10) == 0) {
        char notif[32];
        snprintf(notif, sizeof(notif), "ESTADO:%s", p + 10);
        send_ble_notify(notif);
        strcpy(resp, "OK+NOTIFIED");
        interceptado = true;

    } else if (strcmp(p, "AT") == 0) {
        strcpy(resp, "OK");
        interceptado = true;

    } else if (strcmp(p, "AT+INQ") == 0) {
        strcpy(resp, "OK+INQ_DONE");
        interceptado = true;

    } else if (strcmp(p, "AT+RSSI") == 0) {
        /* Reportar RSSI del primer cliente conectado */
        if (g_conn_count > 0) {
            for (int i = 0; i < MAX_CONNS; i++) {
                if (g_conn_handles[i] != BLE_HS_CONN_HANDLE_NONE) {
                    int8_t rssi = 0;
                    if (ble_gap_conn_rssi(g_conn_handles[i], &rssi) == 0)
                        snprintf(resp, sizeof(resp), "OK+RSSI:%d", rssi);
                    else
                        strcpy(resp, "ERR+RSSI_READ");
                    break;
                }
            }
        } else {
            strcpy(resp, "OK+RSSI:-100");
        }
        interceptado = true;
    }

    if (!interceptado) {
        if (g_conn_count > 0) {
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
        ESP_LOGI(TAG, "[TX → KL25Z]: %s", resp);
    } else {
        uart_send(UART_PC, line);
        ESP_LOGI(TAG, "[TX → PC]: %s", resp);
    }
}

/* ─── Tareas de UART ─────────────────────────────────────────────────────── */
static void uart_kl_task(void *arg) {
    uint8_t byte;
    char    buf[128];
    int     idx = 0;
    for (;;) {
        if (uart_read_bytes(UART_KL, &byte, 1, portMAX_DELAY) > 0) {
            if (byte == '\n' || byte == '\r') {
                buf[idx] = '\0';
                if (idx > 0) process_command(buf, true);
                idx = 0;
            } else if (idx < (int)sizeof(buf) - 1) {
                buf[idx++] = (char)byte;
            }
        }
    }
}

static void uart_pc_task(void *arg) {
    uint8_t byte;
    char    buf[128];
    int     idx = 0;
    for (;;) {
        if (uart_read_bytes(UART_PC, &byte, 1, portMAX_DELAY) > 0) {
            if (byte == '\n' || byte == '\r') {
                buf[idx] = '\0';
                if (idx > 0) process_command(buf, false);
                idx = 0;
            } else if (idx < (int)sizeof(buf) - 1) {
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
    /* Inicializar array de conexiones */
    for (int i = 0; i < MAX_CONNS; i++)
        g_conn_handles[i] = BLE_HS_CONN_HANDLE_NONE;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_config_t led_cfg = { .pin_bit_mask = (1ULL << LED_PIN), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&led_cfg);
    led_set(false);

    uart_config_t uart_kl_cfg = {
        .baud_rate  = 9600, .data_bits = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_KL, UART_BUF, UART_BUF, 0, NULL, 0);
    uart_param_config(UART_KL, &uart_kl_cfg);
    uart_set_pin(UART_KL, TX_KL, RX_KL, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PC, UART_BUF, UART_BUF, 0, NULL, 0);

    nimble_port_init();
    ble_svc_gap_device_name_set("Aspiradora_Ardo");
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(nimble_host_task);

    xTaskCreate(uart_kl_task, "uart_kl", 4096, NULL, 5, NULL);
    xTaskCreate(uart_pc_task, "uart_pc", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Servidor GATT activo. Acepta hasta %d clientes BLE.", MAX_CONNS);
}
