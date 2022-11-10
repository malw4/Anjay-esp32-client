/*
 * Copyright 2021-2022 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <string.h>

#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include <anjay/anjay.h>
#include <anjay/attr_storage.h>
#include <anjay/core.h>
#include <anjay/security.h>
#include <anjay/server.h>
#include <avsystem/commons/avs_log.h>

#include "connect.h"
#include "default_config.h"
#include "firmware_update.h"
#include "lcd.h"
#include "main.h"
#include "objects/objects.h"

#include "firmware_update.h"
#include "objects/objects.h"

#include "bulbulator.h"

#include "driver/rmt.h"
#include "led_strip.h"


static const char *TAG = "example";

#define RMT_TX_CHANNEL RMT_CHANNEL_0

#define EXAMPLE_CHASE_SPEED_MS (100)

/**
 * @brief Simple helper function, converting HSV color space to RGB color space
 *
 * Wiki: https://en.wikipedia.org/wiki/HSL_and_HSV
 *
 */
void led_strip_hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h %= 360; // h -> [0,360]
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    // RGB adjustment amount by hue
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

led_strip_t *strip;
bulbulator_state_t bulb_state = BULBULATOR_IDLE;

void set_strip_color(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < CONFIG_EXAMPLE_STRIP_LED_NUMBER; i++) {
        strip->set_pixel(strip, i, r, g, b);
    }
    strip->refresh(strip, 100);
}
static void led_task(void *pvParameters)
{
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    uint16_t hue = 0;
    uint16_t start_rgb = 0;

    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(CONFIG_EXAMPLE_RMT_TX_GPIO, RMT_TX_CHANNEL);
    // set counter clock to 40MHz
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    // install ws2812 driver
    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(CONFIG_EXAMPLE_STRIP_LED_NUMBER, (led_strip_dev_t)config.channel);
    strip = led_strip_new_rmt_ws2812(&strip_config);
    if (!strip) {
        ESP_LOGE(TAG, "install WS2812 driver failed");
    }
    // Clear LED strip (turn off all LEDs)
    ESP_ERROR_CHECK(strip->clear(strip, 100));
    // Show simple rainbow chasing pattern
    ESP_LOGI(TAG, "LED Rainbow Chase Start");
    while (true) {
        switch (bulb_state) {
            case BULBULATOR_IDLE:
                for (int i = 0; i < 3; i++) {
                    for (int j = i; j < CONFIG_EXAMPLE_STRIP_LED_NUMBER; j++) {
                        // Build RGB values
                        hue = j * 360 / CONFIG_EXAMPLE_STRIP_LED_NUMBER + start_rgb;
                        led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
                        // Write RGB values to strip driver
                        ESP_ERROR_CHECK(strip->set_pixel(strip, j, red, green, blue));
                    }
                    // Flush RGB values to LEDs
                    ESP_ERROR_CHECK(strip->refresh(strip, 100));
                    vTaskDelay(pdMS_TO_TICKS(EXAMPLE_CHASE_SPEED_MS));
                    // strip->clear(strip, 50);
                    // vTaskDelay(pdMS_TO_TICKS(EXAMPLE_CHASE_SPEED_MS));
                }
                start_rgb += 60;
                break;

            case BULBULATOR_START:
                set_strip_color(0xFF, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                set_strip_color(0xFF, 0xFF, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                set_strip_color(0, 0xFF, 0);
                bulb_state = BULBULATOR_MEASURE;
                gpio_intr_enable(FLOW_PIN);
                break;

            case BULBULATOR_MEASURE:
                vTaskDelay(pdMS_TO_TICKS(10000));
                gpio_intr_disable(FLOW_PIN);
                set_strip_color(0xFF, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                bulb_state = BULBULATOR_IDLE;
                break;


            default:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}




#ifdef CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE
#    include "cellular_anjay_impl/cellular_event_loop.h"
#    include "cellular_anjay_impl/net_impl.h"
#    include "cellular_setup.h"
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE

#ifdef CONFIG_ANJAY_SECURITY_MODE_CERTIFICATES
extern const uint8_t CLIENT_PRIVATE_KEY[] asm("_binary_client_key_der_start");
extern const uint32_t CLIENT_PRIVATE_KEY_LEN asm("client_key_der_length");
extern const uint8_t CLIENT_CERT[] asm("_binary_client_cert_der_start");
extern const uint32_t CLIENT_CERT_LEN asm("client_cert_der_length");
extern const uint8_t SERVER_CERT[] asm("_binary_server_cert_der_start");
extern const uint32_t SERVER_CERT_LEN asm("server_cert_der_length");
#else
static char PSK[ANJAY_MAX_SECRET_KEY_SIZE];
static char IDENTITY[ANJAY_MAX_PK_OR_IDENTITY_SIZE];
#endif // CONFIG_ANJAY_SECURITY_MODE_CERTIFICATES

#ifdef CONFIG_ANJAY_CLIENT_SOCKET_TCP
#    define MAIN_PREFERRED_TRANSPORT "T"
#else
#    define MAIN_PREFERRED_TRANSPORT "U"
#endif // CONFIG_ANJAY_CLIENT_TCP_SOCKET

static char SERVER_URI[ANJAY_MAX_PK_OR_IDENTITY_SIZE];
static char ENDPOINT_NAME[ANJAY_MAX_PK_OR_IDENTITY_SIZE];

static const anjay_dm_object_def_t **DEVICE_OBJ;
static const anjay_dm_object_def_t **PUSH_BUTTON_OBJ;
static const anjay_dm_object_def_t **WATER_METER_OBJ;
static const anjay_dm_object_def_t **LIGHT_CONTROL_OBJ;
#ifdef CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI
static const anjay_dm_object_def_t **WLAN_OBJ;
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI

static anjay_t *anjay;
static avs_sched_handle_t sensors_job_handle;
static avs_sched_handle_t connection_status_job_handle;
#ifdef CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI
static avs_sched_handle_t change_config_job_handle;
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI

static int read_anjay_config();
#ifdef CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI
static void change_config_job(avs_sched_t *sched, const void *args_ptr);

void schedule_change_config() {
    AVS_SCHED_NOW(anjay_get_scheduler(anjay), &change_config_job_handle,
                  change_config_job, NULL, 0);
}

static int connect_to_instance(wifi_instance_t iid) {
    wifi_config_t wifi_config =
            wlan_object_get_instance_wifi_config(WLAN_OBJ, iid);
    return connect_internal(&wifi_config);
}

// Reconfigure wifi due to enable resource value change
static void change_config_job(avs_sched_t *sched, const void *args_ptr) {
    bool preconf_inst_enable = false, writable_inst_enable = false;

    disconnect_internal();
    if (wlan_object_is_instance_enabled(WLAN_OBJ,
                                        ANJAY_WIFI_OBJ_WRITABLE_INSTANCE)) {
        avs_log(tutorial, INFO,
                "Trying to connect to wifi with configuration from server...");
        if (connect_to_instance(ANJAY_WIFI_OBJ_WRITABLE_INSTANCE)) {
            avs_log(tutorial, INFO,
                    "connection unsuccessful, trying to connect to wifi with "
                    "configuration from NVS");
            connect_to_instance(ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE);
            wlan_object_set_writable_iface_failed(anjay, WLAN_OBJ, true);
            preconf_inst_enable = true;
        } else {
            avs_log(tutorial, INFO, "connection successful");
            wlan_object_set_writable_iface_failed(anjay, WLAN_OBJ, false);
            writable_inst_enable = true;
        }
    } else {
        avs_log(tutorial, INFO,
                "Trying to connect to wifi with configuration from NVS...");
        if (!connect_to_instance(ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE)) {
            avs_log(tutorial, INFO, "connection successful");
        }
        preconf_inst_enable = true;
    }

    wlan_object_set_instance_enable(anjay, WLAN_OBJ,
                                    ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE,
                                    preconf_inst_enable);
    wlan_object_set_instance_enable(anjay, WLAN_OBJ,
                                    ANJAY_WIFI_OBJ_WRITABLE_INSTANCE,
                                    writable_inst_enable);

    anjay_transport_schedule_reconnect(anjay, ANJAY_TRANSPORT_SET_IP);
}
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI

// Installs Security Object and adds and instance of it.
// An instance of Security Object provides information needed to connect to
// LwM2M server.
static int setup_security_object(anjay_t *anjay) {
    if (anjay_security_object_install(anjay)) {
        return -1;
    }

    anjay_security_instance_t security_instance = {
        .ssid = 1,
        .server_uri = SERVER_URI,
#if defined(CONFIG_ANJAY_SECURITY_MODE_CERTIFICATES)
        .security_mode = ANJAY_SECURITY_CERTIFICATE,
        .public_cert_or_psk_identity = CLIENT_CERT,
        .public_cert_or_psk_identity_size = CLIENT_CERT_LEN,
        .private_cert_or_psk_key = CLIENT_PRIVATE_KEY,
        .private_cert_or_psk_key_size = CLIENT_PRIVATE_KEY_LEN,
        .server_public_key = SERVER_CERT,
        .server_public_key_size = SERVER_CERT_LEN
#elif defined(CONFIG_ANJAY_SECURITY_MODE_PSK)
        .security_mode = ANJAY_SECURITY_PSK,
        .public_cert_or_psk_identity = (const uint8_t *) IDENTITY,
        .public_cert_or_psk_identity_size = strlen(IDENTITY),
        .private_cert_or_psk_key = (const uint8_t *) PSK,
        .private_cert_or_psk_key_size = strlen(PSK)
#else
        .security_mode = ANJAY_SECURITY_NOSEC
#endif // CONFIG_ANJAY_SECURITY_MODE_CERTIFICATES
    };

    // Anjay will assign Instance ID automatically
    anjay_iid_t security_instance_id = ANJAY_ID_INVALID;
    if (anjay_security_object_add_instance(anjay, &security_instance,
                                           &security_instance_id)) {
        return -1;
    }

    return 0;
}

// Installs Server Object and adds and instance of it.
// An instance of Server Object provides the data related to a LwM2M Server.
static int setup_server_object(anjay_t *anjay) {
    if (anjay_server_object_install(anjay)) {
        return -1;
    }

    const anjay_server_instance_t server_instance = {
        // Server Short ID
        .ssid = 1,
        // Client will send Update message often than every 60 seconds
        .lifetime = 60,
        // Disable Default Minimum Period resource
        .default_min_period = -1,
        // Disable Default Maximum Period resource
        .default_max_period = -1,
        // Disable Disable Timeout resource
        .disable_timeout = -1,
        // Sets preferred transport
        .binding = MAIN_PREFERRED_TRANSPORT
    };

    // Anjay will assign Instance ID automatically
    anjay_iid_t server_instance_id = ANJAY_ID_INVALID;
    if (anjay_server_object_add_instance(anjay, &server_instance,
                                         &server_instance_id)) {
        return -1;
    }

    return 0;
}

static void update_objects_job(avs_sched_t *sched, const void *anjay_ptr) {
    anjay_t *anjay = *(anjay_t *const *) anjay_ptr;

    device_object_update(anjay, DEVICE_OBJ);
    push_button_object_update(anjay, PUSH_BUTTON_OBJ);
    sensors_update(anjay);

    AVS_SCHED_DELAYED(sched, &sensors_job_handle,
                      avs_time_duration_from_scalar(1, AVS_TIME_S),
                      &update_objects_job, &anjay, sizeof(anjay));
}

#if CONFIG_ANJAY_CLIENT_LCD
static void check_and_write_connection_status(anjay_t *anjay) {
    if (anjay_get_socket_entries(anjay) == NULL) {
        lcd_write_connection_status(LCD_CONNECTION_STATUS_DISCONNECTED);
    } else if (anjay_all_connections_failed(anjay)) {
        lcd_write_connection_status(LCD_CONNECTION_STATUS_CONNECTION_ERROR);
    } else if (anjay_ongoing_registration_exists(anjay)) {
        lcd_write_connection_status(LCD_CONNECTION_STATUS_CONNECTING);
    } else {
        lcd_write_connection_status(LCD_CONNECTION_STATUS_CONNECTED);
    }
}
#endif // CONFIG_ANJAY_CLIENT_LCD

static void update_connection_status_job(avs_sched_t *sched,
                                         const void *anjay_ptr) {
    anjay_t *anjay = *(anjay_t *const *) anjay_ptr;
#if CONFIG_ANJAY_CLIENT_LCD
    check_and_write_connection_status(anjay);
#endif // CONFIG_ANJAY_CLIENT_LCD

    static bool connected_prev = true;
    bool err;

#if defined(CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE)
    CellularServiceStatus_t service_status = { 0 };
    err = (bool) Cellular_GetServiceStatus(CellularHandle, &service_status);

    if (!err) {
        err = (service_status.csRegistrationStatus
                       != REGISTRATION_STATUS_REGISTERED_HOME
               && service_status.csRegistrationStatus
                          != REGISTRATION_STATUS_ROAMING_REGISTERED
               && service_status.psRegistrationStatus
                          != REGISTRATION_STATUS_REGISTERED_HOME
               && service_status.psRegistrationStatus
                          != REGISTRATION_STATUS_ROAMING_REGISTERED);
    }
#elif defined(CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI)
    wifi_ap_record_t ap_info;
    err = (bool) esp_wifi_sta_get_ap_info(&ap_info);
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE

    if (connected_prev && err) {
        connected_prev = false;
        anjay_transport_enter_offline(anjay, ANJAY_TRANSPORT_SET_IP);
    } else if (!connected_prev && !err) {
        anjay_transport_exit_offline(anjay, ANJAY_TRANSPORT_SET_IP);
        connected_prev = true;
    }

    AVS_SCHED_DELAYED(sched, &connection_status_job_handle,
                      avs_time_duration_from_scalar(1, AVS_TIME_S),
                      update_connection_status_job, &anjay, sizeof(anjay));
}

static void anjay_init(void) {
    const anjay_configuration_t CONFIG = {
        .endpoint_name = ENDPOINT_NAME,
        .in_buffer_size = 4000,
        .out_buffer_size = 4000,
        .msg_cache_size = 4000
    };

    // Read necessary data for object install
    read_anjay_config();

    anjay = anjay_new(&CONFIG);
    if (!anjay) {
        avs_log(tutorial, ERROR, "Could not create Anjay object");
        return;
    }

    // Install Attribute storage and setup necessary objects
    if (setup_security_object(anjay) || setup_server_object(anjay)
            || fw_update_install(anjay)) {
        avs_log(tutorial, ERROR, "Failed to install core objects");
        return;
    }

    if (!(DEVICE_OBJ = device_object_create())
            || anjay_register_object(anjay, DEVICE_OBJ)) {
        avs_log(tutorial, ERROR, "Could not register Device object");
        return;
    }

    if ((LIGHT_CONTROL_OBJ = light_control_object_create())) {
        anjay_register_object(anjay, LIGHT_CONTROL_OBJ);
    }

    if ((PUSH_BUTTON_OBJ = push_button_object_create())) {
        anjay_register_object(anjay, PUSH_BUTTON_OBJ);
    }

    if ((WATER_METER_OBJ = water_meter_object_create())) {
        anjay_register_object(anjay, WATER_METER_OBJ);
    }

#ifdef CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI
    if ((WLAN_OBJ = wlan_object_create())) {
        anjay_register_object(anjay, WLAN_OBJ);
    }
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI
}

static void anjay_task(void *pvParameters) {
    sensors_install(anjay);

    update_connection_status_job(anjay_get_scheduler(anjay), &anjay);
    update_objects_job(anjay_get_scheduler(anjay), &anjay);

#ifdef CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE
    cellular_event_loop_run(anjay);
#else
    anjay_event_loop_run(anjay, avs_time_duration_from_scalar(1, AVS_TIME_S));
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE
    avs_sched_del(&sensors_job_handle);
    avs_sched_del(&connection_status_job_handle);
    anjay_delete(anjay);
    sensors_release();

    if (fw_update_requested()) {
        fw_update_reboot();
    }
}

static void
log_handler(avs_log_level_t level, const char *module, const char *msg) {
    esp_log_level_t esp_level = ESP_LOG_NONE;
    switch (level) {
    case AVS_LOG_QUIET:
        esp_level = ESP_LOG_NONE;
        break;
    case AVS_LOG_ERROR:
        esp_level = ESP_LOG_ERROR;
        break;
    case AVS_LOG_WARNING:
        esp_level = ESP_LOG_WARN;
        break;
    case AVS_LOG_INFO:
        esp_level = ESP_LOG_INFO;
        break;
    case AVS_LOG_DEBUG:
        esp_level = ESP_LOG_DEBUG;
        break;
    case AVS_LOG_TRACE:
        esp_level = ESP_LOG_VERBOSE;
        break;
    }
    ESP_LOG_LEVEL_LOCAL(esp_level, "anjay", "%s", msg);
}

static int read_nvs_anjay_config(void) {
    nvs_handle_t nvs_h;

    if (nvs_open(MAIN_NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_h)) {
        return -1;
    }

    int result = (nvs_get_str(nvs_h, "uri", SERVER_URI,
                              &(size_t) { sizeof(SERVER_URI) })
                  || nvs_get_str(nvs_h, "endpoint_name", ENDPOINT_NAME,
                                 &(size_t) { sizeof(ENDPOINT_NAME) })
#ifdef CONFIG_ANJAY_SECURITY_MODE_PSK
                  || nvs_get_str(nvs_h, "psk", PSK, &(size_t) { sizeof(PSK) })
                  || nvs_get_str(nvs_h, "identity", IDENTITY,
                                 &(size_t) { sizeof(IDENTITY) })
#endif // CONFIG_ANJAY_SECURITY_MODE_PSK
                          )
                         ? -1
                         : 0;
    nvs_close(nvs_h);

    return result;
}

static int read_anjay_config(void) {
    int err = 0;
    avs_log(tutorial, INFO, "Opening (NVS) read-only handle... ");
    if (read_nvs_anjay_config()) {
        avs_log(tutorial, WARNING,
                "Reading from NVS has failed, attempt with Kconfig");
        snprintf(ENDPOINT_NAME, sizeof(ENDPOINT_NAME), "%s",
                 CONFIG_ANJAY_CLIENT_ENDPOINT_NAME);
        snprintf(SERVER_URI, sizeof(SERVER_URI), "%s",
                 CONFIG_ANJAY_CLIENT_SERVER_URI);
#ifdef CONFIG_ANJAY_SECURITY_MODE_PSK
        snprintf(PSK, sizeof(PSK), "%s", CONFIG_ANJAY_CLIENT_PSK_KEY);
        snprintf(IDENTITY, sizeof(IDENTITY), "%s",
                 CONFIG_ANJAY_CLIENT_PSK_IDENTITY);
#endif // CONFIG_ANJAY_SECURITY_MODE_PSK
        err = -1;
    } else {
        printf("Read Anjay setup from NVS:\n");
    }
    printf("Endpoint:   %s\n", ENDPOINT_NAME);
    printf("PSK:        %s\n", PSK);
    printf("Identity:   %s\n", IDENTITY);
    printf("Server Uri: %s\n", SERVER_URI);

    return err;
}

#ifdef CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI
static int read_nvs_wifi_config(const char *namespace,
                                wifi_config_t *wifi_config,
                                uint8_t *en) {
    nvs_handle_t nvs_h;

    if (nvs_open(namespace, NVS_READONLY, &nvs_h)) {
        return -1;
    }

    int result =
            (nvs_get_str(nvs_h, MAIN_NVS_WIFI_SSID_KEY,
                         (char *) wifi_config->sta.ssid,
                         &(size_t) { sizeof(wifi_config->sta.ssid) })
             || nvs_get_str(nvs_h, MAIN_NVS_WIFI_PASSWORD_KEY,
                            (char *) wifi_config->sta.password,
                            &(size_t) { sizeof(wifi_config->sta.password) })
             || nvs_get_u8(nvs_h, MAIN_NVS_ENABLE_KEY, en))
                    ? -1
                    : 0;
    nvs_close(nvs_h);

    return result;
}

static int read_wifi_config(void) {
    wifi_config_t preconf_wifi_config = { 0 };
    uint8_t preconf_en = 0;
    wifi_config_t writable_wifi_config = { 0 };
    uint8_t writable_en = 0;
    int err = 0;

    avs_log(tutorial, INFO, "Opening (NVS) read-only handle... ");
    if (read_nvs_wifi_config(MAIN_NVS_CONFIG_NAMESPACE, &preconf_wifi_config,
                             &preconf_en)) {
        avs_log(tutorial, WARNING,
                "Reading from NVS has failed, attempt with Kconfig");

        snprintf((char *) preconf_wifi_config.sta.ssid,
                 sizeof(preconf_wifi_config.sta.ssid), "%s",
                 CONFIG_ANJAY_WIFI_SSID);
        snprintf((char *) preconf_wifi_config.sta.password,
                 sizeof(preconf_wifi_config.sta.password), "%s",
                 CONFIG_ANJAY_WIFI_PASSWORD);

        err = -1;
    }
    printf("Preconfigured WiFi:\n");
    printf("SSID: %s\n", preconf_wifi_config.sta.ssid);
    printf("Pass: %s\n", preconf_wifi_config.sta.password);
    printf("Enabled: %d", preconf_en);

    avs_log(tutorial, INFO,
            "Opening Non-Volatile Storage (NVS) with wifi writable "
            "configuration handle... ");
    if (read_nvs_wifi_config(MAIN_NVS_WRITABLE_WIFI_CONFIG_NAMESPACE,
                             &writable_wifi_config, &writable_en)) {
        avs_log(tutorial, WARNING, "Reading from NVS has failed");

        err = -1;
    } else {

        printf("Writable WiFi:\n");
        printf("SSID: %s\n", writable_wifi_config.sta.ssid);
        printf("Pass: %s\n", writable_wifi_config.sta.password);
        printf("Enabled: %d", writable_en);
    }

    preconf_wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    writable_wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wlan_object_set_instance_wifi_config(anjay, WLAN_OBJ,
                                         ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE,
                                         &preconf_wifi_config);
    wlan_object_set_instance_wifi_config(anjay, WLAN_OBJ,
                                         ANJAY_WIFI_OBJ_WRITABLE_INSTANCE,
                                         &writable_wifi_config);
    wlan_object_set_instance_enable(anjay, WLAN_OBJ,
                                    ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE,
                                    (bool) preconf_en);
    wlan_object_set_instance_enable(anjay, WLAN_OBJ,
                                    ANJAY_WIFI_OBJ_WRITABLE_INSTANCE,
                                    (bool) writable_en);
    return err;
}

static int wifi_config_check(wifi_config_t *wifi_config) {
    if ((strlen((char *) wifi_config->sta.ssid)) == 0) {
        return -1;
    }
    return 0;
}

static void set_wifi_config(wifi_config_t *wifi_config) {
    if (wlan_object_is_instance_enabled(WLAN_OBJ,
                                        ANJAY_WIFI_OBJ_WRITABLE_INSTANCE)) {
        // get wifi configuration from writable instance
        *wifi_config = wlan_object_get_instance_wifi_config(
                WLAN_OBJ, ANJAY_WIFI_OBJ_WRITABLE_INSTANCE);
        // check this configuration
        if (wifi_config_check(wifi_config)) {
            // if first configuration is incorrect, get another one from
            // preconfigured instance
            *wifi_config = wlan_object_get_instance_wifi_config(
                    WLAN_OBJ, ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE);
            wlan_object_set_instance_enable(
                    anjay, WLAN_OBJ, ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE,
                    true);
            wlan_object_set_instance_enable(
                    anjay, WLAN_OBJ, ANJAY_WIFI_OBJ_WRITABLE_INSTANCE, false);
            avs_log(tutorial, INFO,
                    "Using wifi configuration from preconfigured instance");
        } else {
            avs_log(tutorial, INFO,
                    "Using wifi configuration from writable instance");
        }
    } else {
        *wifi_config = wlan_object_get_instance_wifi_config(
                WLAN_OBJ, ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE);
        avs_log(tutorial, INFO,
                "Using wifi configuration from preconfigured instance");
    }
}
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI

void app_main(void) {
    gpio_install_isr_service(0);
    xTaskCreate(&led_task, "led_task", 16384, NULL, 5, NULL);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    avs_log_set_handler(log_handler);

    avs_log_set_default_level(AVS_LOG_TRACE);
    anjay_init();

#if CONFIG_ANJAY_CLIENT_LCD
    lcd_init();
#    if defined(CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE)
    lcd_write_connection_status(LCD_CONNECTION_STATUS_BG96_SETTING);
#    elif defined(CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI)
    lcd_write_connection_status(LCD_CONNECTION_STATUS_WIFI_CONNECTING);
#    endif // CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE
#endif     // CONFIG_ANJAY_CLIENT_LCD

#if defined(CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE)
    while (!setupCellular()) {
        avs_log(tutorial, WARNING, "Cellular setup has failed");
        Cellular_Cleanup(CellularHandle);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#elif defined(CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI)
    read_wifi_config();

    wifi_config_t wifi_config = { 0 };
    set_wifi_config(&wifi_config);

    if (connect_internal(&wifi_config)) {
        wifi_config = wlan_object_get_instance_wifi_config(
                WLAN_OBJ, ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE);
        while (connect_internal(&wifi_config)) {
            avs_log(tutorial, WARNING,
                    "Connection attempt to preconfigured wifi has failed, "
                    "reconnection in progress...");
        }
        wlan_object_set_instance_enable(
                anjay, WLAN_OBJ, ANJAY_WIFI_OBJ_PRECONFIGURED_INSTANCE, true);
        wlan_object_set_instance_enable(
                anjay, WLAN_OBJ, ANJAY_WIFI_OBJ_WRITABLE_INSTANCE, false);
        wlan_object_set_writable_iface_failed(anjay, WLAN_OBJ, true);
    }
#endif // CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE

#if CONFIG_ANJAY_CLIENT_LCD
#    if defined(CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE)
    lcd_write_connection_status(LCD_CONNECTION_STATUS_BG96_SET);
#    elif defined(CONFIG_ANJAY_CLIENT_INTERFACE_ONBOARD_WIFI)
    lcd_write_connection_status(LCD_CONNECTION_STATUS_WIFI_CONNECTED);
#    endif // CONFIG_ANJAY_CLIENT_INTERFACE_BG96_MODULE
#endif     // CONFIG_ANJAY_CLIENT_LCD

    xTaskCreate(&anjay_task, "anjay_task", 16384, NULL, 5, NULL);
}
