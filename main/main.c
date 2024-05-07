#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "dht.h"

#include <time.h>
#include <sys/time.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "nvs.h"

#include "driver/gpio.h"
#include "driver/adc.h"

static const char *TAG = "temp_sensor";

#define WIFI_SSID             CONFIG_WIFI_SSID
#define WIFI_PASS             CONFIG_WIFI_PASSWORD
#define MAX_RETRY         CONFIG_MAXIMUM_RETRY
#define STATIC_IP_ADDR        CONFIG_STATIC_IP_ADDR
#define STATIC_NETMASK_ADDR   CONFIG_STATIC_NETMASK_ADDR
#define STATIC_GW_ADDR        CONFIG_STATIC_GW_ADDR
#ifdef CONFIG_STATIC_DNS_AUTO
#define MAIN_DNS_SERVER       STATIC_GW_ADDR
#define BACKUP_DNS_SERVER     "0.0.0.0"
#else
#define MAIN_DNS_SERVER       CONFIG_STATIC_DNS_SERVER_MAIN
#define BACKUP_DNS_SERVER     CONFIG_STATIC_DNS_SERVER_BACKUP
#endif
#ifdef CONFIG_STATIC_DNS_RESOLVE_TEST
#define RESOLVE_DOMAIN        CONFIG_STATIC_RESOLVE_DOMAIN
#endif

static int retry_cnt = 0;

#define MQTT_PUB_TEMP_DHT "benjamins_bedroom/dht/temperature"
#define MQTT_PUB_HUM_DHT "benjamins_bedroom/dht/humidity"

uint32_t MQTT_CONNECTED = 0;

#define SENSOR_TYPE DHT_TYPE_DHT11
#define DHT_GPIO_PIN GPIO_NUM_10

static RTC_DATA_ATTR struct timeval sleep_enter_time;

static void mqtt_app_start(void);

static esp_err_t wifi_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        ESP_LOGI(TAG, "Trying to connect with Wi-Fi\n");
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Wi-Fi connected\n");
        break;

    case IP_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "got ip: starting MQTT Client\n");
        mqtt_app_start();
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "disconnected: Retrying Wi-Fi\n");
        if (retry_cnt++ < MAX_RETRY)
        {
            esp_wifi_connect();
        }
        else
            ESP_LOGI(TAG, "Max Retry Failed: Wi-Fi Connection\n");
        break;

    default:
        break;
    }
    return ESP_OK;
}

void wifi_init(void)
{
    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_netif_init();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        MQTT_CONNECTED = 1;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        MQTT_CONNECTED = 0;
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

esp_mqtt_client_handle_t client = NULL;
static void mqtt_app_start(void)
{
    ESP_LOGI(TAG, "STARTING MQTT");
    esp_mqtt_client_config_t mqttConfig = {
        .broker.address.uri = "mqtt://mqtt-user:Q6pK6cUr@192.168.0.12:1883"};

    client = esp_mqtt_client_init(&mqttConfig);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

void DHT_Publisher_task(void *pvParameter) {

	float temp, hum;

    while (1) {
        if (MQTT_CONNECTED)
        {
            if (dht_read_float_data(SENSOR_TYPE, DHT_GPIO_PIN, &hum, &temp) == ESP_OK) {

                char humidity[12];
                sprintf(humidity, "%.2f", hum);

                char temperature[12];
                sprintf(temperature, "%.2f", temp);

                printf("Humidity: %.1f%% Temp: %.1fC\n", hum, temp);

                esp_mqtt_client_publish(client, MQTT_PUB_HUM_DHT, humidity, 0, 0, 0);
                ESP_LOGI(TAG, "Humidity sent to MQTT Broker");

                esp_mqtt_client_publish(client, MQTT_PUB_TEMP_DHT, temperature, 0, 0, 0);
                ESP_LOGI(TAG, "Temperature sent to MQTT Broker");

                const int wakeup_time_sec = 290;
                printf("Enabling timer wakeup, %ds\n", wakeup_time_sec);
                esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000);
        
                printf("Entering deep sleep\n");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_deep_sleep_start();


            } else 
            {
                printf("Could not read data from sensor\n");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            
        } else
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }   
    }
}

void app_main() {
    struct timeval now;
    gettimeofday(&now, NULL);
    int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 + (now.tv_usec - sleep_enter_time.tv_usec) / 1000;

    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER: {
            printf("Wake up from timer. Time spent in deep sleep: %dms\n", sleep_time_ms);
            break;
        }
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            printf("Not a deep sleep reset\n");
    }

	nvs_flash_init();
    wifi_init();
	xTaskCreate(&DHT_Publisher_task, "DHT_Publisher_task", 2048, NULL, 5, NULL );
}
