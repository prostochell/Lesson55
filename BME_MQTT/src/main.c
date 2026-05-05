#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_random.h" // Required for the esp_random() function

// --- WI-FI CONFIGURATION ---
#define WIFI_SSID "TELPOL-26858"
#define WIFI_PASS "aygmu893p4"
#define MAXIMUM_RETRY 5

static const char *TAG = "MQTT_MOCK";

// Wi-Fi synchronization events
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
static int s_retry_num = 0;

// Global MQTT client handle
esp_mqtt_client_handle_t mqtt_client;

// --- 1. WI-FI EVENT HANDLER ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to Wi-Fi...");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize Wi-Fi in Station Mode
void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

// --- 2. MQTT EVENT HANDLER ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Successfully connected to MQTT broker");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Message published successfully (msg_id=%d)", event->msg_id);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error occurred");
        break;
    default:
        break;
    }
}

// --- 3. MOCK DATA PUBLISH TASK ---
void telemetry_task(void *pvParameters)
{
    float temperature, pressure, humidity;

    while (1)
    {
        // Generate mock data since physical sensor is currently unavailable
        // Temperature between 20.0 and 29.9 C
        temperature = 20.0 + (esp_random() % 100) / 10.0;

        // Pressure between 1000.0 and 1049.9 hPa
        pressure = 1000.0 + (esp_random() % 500) / 10.0;

        // Humidity between 40.0 and 59.9 %
        humidity = 40.0 + (esp_random() % 200) / 10.0;

        // Format data into a JSON object
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "temperature", temperature);
        cJSON_AddNumberToObject(root, "pressure", pressure);
        cJSON_AddNumberToObject(root, "humidity", humidity);

        // Generate a string from the JSON object
        char *json_string = cJSON_PrintUnformatted(root);

        if (json_string != NULL)
        {
            ESP_LOGI(TAG, "Publishing mock telemetry: %s", json_string);

            // Publish to the MQTT broker (QoS 0)
            esp_mqtt_client_publish(mqtt_client, "esp32s3/telemetry/bme", json_string, 0, 0, 0);

            // Free the string memory
            free(json_string);
        }

        // Free the JSON object memory
        cJSON_Delete(root);

        // Wait 5 seconds before generating the next reading
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// --- 4. MAIN ENTRY POINT ---
void app_main(void)
{
    // 1. Initialize NVS (Required for Wi-Fi credentials storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Wi-Fi
    wifi_init_sta();

    // Note: I2C and BMP280 initialization code has been removed
    // to prevent hardware conflicts while using mock data.

    // 3. Initialize and start MQTT Client
    ESP_LOGI(TAG, "Starting MQTT Client...");
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.emqx.io:1883", 
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    // 4. Start the mock telemetry FreeRTOS task
    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL);
}