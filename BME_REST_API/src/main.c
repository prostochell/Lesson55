#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "bmp280.h"
#include "i2cdev.h"

#define WIFI_SSID "TELPOL-26858"
#define WIFI_PASS "aygmu893p4"
#define MAXIMUM_RETRY 5

#define I2C_MASTER_SCL_IO 9       
#define I2C_MASTER_SDA_IO 8      
#define I2C_MASTER_NUM 0        
#define I2C_MASTER_FREQ_HZ 100000 
#define I2C_MASTER_TX_BUF_DISABLE 0
#define I2C_MASTER_RX_BUF_DISABLE 0

bmp280_t bme280_dev;

static const char *TAG = "REST_API_BME280";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;


void read_bme280_data(float *temp, float *press)
{
    *temp = 22.0 + (esp_random() % 50) / 10.0;
    *press = 1005.0 + (esp_random() % 200) / 10.0;
}

static esp_err_t get_sensors_handler(httpd_req_t *req)
{
    float temp, press;
    read_bme280_data(&temp, &press);

    ESP_LOGI(TAG, "Отримано GET запит на /api/sensors");

    // Формування JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temperature", temp);
    cJSON_AddNumberToObject(root, "pressure", press);
    cJSON_AddStringToObject(root, "unit_temp", "C");
    cJSON_AddStringToObject(root, "unit_press", "hPa");
    cJSON_AddStringToObject(root, "status", "OK");

    char *json_string = cJSON_PrintUnformatted(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t res = httpd_resp_send(req, json_string, HTTPD_RESP_USE_STRLEN);

    free(json_string);
    cJSON_Delete(root);

    return res;
}

static const httpd_uri_t api_sensors = {
    .uri = "/api/sensors",
    .method = HTTP_GET,
    .handler = get_sensors_handler,
    .user_ctx = NULL};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    ESP_LOGI(TAG, "Запуск HTTP сервера на порту: %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &api_sensors);
        return server;
    }

    ESP_LOGE(TAG, "Помилка запуску HTTP сервера!");
    return NULL;
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
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
            ESP_LOGI(TAG, "Спроба підключення до Wi-Fi знову...");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Не вдалося підключитися до Wi-Fi");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Отримано IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Очікування підключення до Wi-Fi...");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Успішно підключено до SSID: %s", WIFI_SSID);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Помилка підключення до SSID: %s", WIFI_SSID);
    }
    else
    {
        ESP_LOGE(TAG, "Непередбачена помилка Wi-Fi");
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    ESP_ERROR_CHECK(i2cdev_init());

    ESP_LOGI(TAG, "Ініціалізація датчика BME280...");

    bmp280_params_t bme280_params;
    bmp280_init_default_params(&bme280_params);

    memset(&bme280_dev, 0, sizeof(bmp280_t));

    // BMP280_I2C_ADDRESS_0 (0x76) or BMP280_I2C_ADDRESS_1 (0x77)
    esp_err_t res = bmp280_init_desc(&bme280_dev, BMP280_I2C_ADDRESS_1, I2C_MASTER_NUM, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    if (res != ESP_OK)
    {
        ESP_LOGE(TAG, "Не вдалося ініціалізувати дескриптор датчика");
    }

    res = bmp280_init(&bme280_dev, &bme280_params);
    if (res != ESP_OK)
    {
        ESP_LOGE(TAG, "Не вдалося знайти BME280! Перевірте підключення та адресу (0x76/0x77)");
    }
    else
    {
        bool bme280p = bme280_dev.id == BME280_CHIP_ID;
        ESP_LOGI(TAG, "Успішно знайдено датчик: %s", bme280p ? "BME280" : "BMP280");
    }
    ESP_LOGI(TAG, "Запуск Wi-Fi...");
    wifi_init_sta();

    start_webserver();
}