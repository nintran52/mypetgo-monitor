/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "define.h"
#include "driver/gpio.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY CONFIG_ESP_MAXIMUM_RETRY
#define RESPONSE_TIMEOUT_MS (30000)

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi station";
static const char *TAG_GPS = "GNSS";

static int s_retry_num = 0;

static QueueHandle_t uart0_queue;
#define BUF_SIZE (1024)
bool f_response_at_cmd = false;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

static void parsedateTime(char *dateTime, char *data)
{
    dateTime[0] = data[0];
    dateTime[1] = data[1];
    dateTime[2] = ':';
    dateTime[3] = data[2];
    dateTime[4] = data[3];
    dateTime[5] = ':';
    dateTime[6] = data[4];
    dateTime[7] = data[5];
}

struct Gps
{
    char time[17];
    double latitude;
    double longitude;
    double altitude;
    double spkm;
    float snr;
};

static struct Gps gps_ = {
    .time = {0},
    .latitude = 0.000,
    .longitude = 0.000,
    .altitude = 0,
    .spkm = 0.0,
    .snr = 0.0,
};

float hdop_min = 2;

static double parseLatLon(char *data)
{
    double raw = strtod(data, NULL);
    int data_int = raw / 100;
    double decimal = raw - data_int * 100;
    return (double)(data_int + decimal / 60);
}

static void gpsParse(char data[13][13], uint8_t size)
{
    // ESP_LOGI(TAG_NET, "--Start parse GPS---");
    double lat_temp = 0;
    double lon_temp = 0;
    char *token;
    token = strtok(data[1], ".");
    if (!token)
    {
        return;
    }
    char time[8];
    parsedateTime(time, token);
    if (!memcmp(data[4], "N", 2))
    {
        lat_temp = parseLatLon(data[3]);
    }
    else if (!memcmp(data[4], "S", 2))
    {
        lat_temp = -parseLatLon(data[3]);
    }
    else
    {
        return;
    }
    if (size > 6)
    {
        if (!memcmp(data[6], "E", 2))
        {
            lon_temp = parseLatLon(data[5]);
        }
        else if (!memcmp(data[6], "W", 2))
        {
            lon_temp = -parseLatLon(data[5]);
        }
        else
        {
            return;
        }
    }

    if (lat_temp != 0 && lon_temp != 0)
    {
        gps_.latitude = lat_temp;
        gps_.longitude = lon_temp;
        gps_.altitude = strtod("1", NULL);
        gps_.spkm = strtod(data[7], NULL);
        gps_.time[8] = ' ';
        memcpy(gps_.time + 9, time, 8);
        ESP_LOGI(TAG_GPS, "GPS Parse -> Lat :%f Long: %f Speed over ground: %f (Km/hr) Date: %s", gps_.latitude, gps_.longitude, gps_.spkm, gps_.time);
    }
    return;
}

static void dataParse(const uint8_t *data, uint32_t size)
{
    char dt[size];
    memcpy(dt, data, size);
    char *tmp_str;
    tmp_str = NULL;

    tmp_str = strstr(dt, "$GPGSV");
    if (tmp_str)
    {
        tmp_str = strtok(tmp_str, "\r\n");
        if (!tmp_str)
        {
            return;
        }
        char gps_info[13][13] = {0};
        uint8_t pos = 0;
        char *pch = strchr(tmp_str, ',');
        int _s = 0;
        while (pch != NULL && pos < 13)
        {
            int len = pch - tmp_str + 1;
            pch = strchr(pch + 1, ',');
            memcpy(gps_info[pos], tmp_str + _s, len - _s - 1);
            _s = len;
            pos++;
        }
        float gps_snr = strtod(gps_info[7], NULL);
        if (gps_snr > 0)
        {
            ESP_LOGI(TAG_GPS, "Signal-to-noise ratio(SNR):%0.f dB", gps_snr);
        }
    }

    // //$GNRMC,054515.00,A,1602.42256,N,10814.79274,E,0.0,,210722,1.1,W,A,V*68
    // //$GPRMC,054518.00,A,1602.42209,N,10814.79374,E,0.0,,210722,1.1,W,A,V*70
    // tmp_str = strstr(dt, "$GNRMC");
    // if (tmp_str)
    // {
    //     // test
    //     tmp_str = strtok(tmp_str, "\r\n");
    //     if (!tmp_str)
    //     {
    //         return;
    //     }
    //     //$GNRMC,,V,,,,,,,,,,N,V*37

    //     char *check_str = strstr(tmp_str, ",,");
    //     if (check_str != NULL)
    //     {
    //         return;
    //     }
    //     ESP_LOGI(TAG_GPS, "gps receive %s", tmp_str);

    //     // test
    //     char gps_info[13][13] = {0};
    //     uint8_t pos = 0;
    //     char *pch = strchr(tmp_str, ',');
    //     int _s = 0;
    //     while (pch != NULL && pos < 13)
    //     {
    //         int len = pch - tmp_str + 1;
    //         pch = strchr(pch + 1, ',');
    //         memcpy(gps_info[pos], tmp_str + _s, len - _s - 1);
    //         _s = len;
    //         pos++;
    //     }
    //     if (pos > 4)
    //     {
    //         gpsParse(gps_info, pos);
    //         return;
    //     }
    //     return;
    // }

    // tmp_str = strstr(dt, "$GPRMC");
    // if (tmp_str)
    // {
    //     // test
    //     tmp_str = strtok(tmp_str, "\r\n");
    //     if (!tmp_str)
    //     {
    //         return;
    //     }
    //     //$GNRMC,,V,,,,,,,,,,N,V*37

    //     char *check_str = strstr(tmp_str, ",,");
    //     if (check_str != NULL)
    //     {
    //         return;
    //     }
    //     ESP_LOGI(TAG_GPS, "gps receive %s", tmp_str);

    //     // test
    //     char gps_info[13][13] = {0};
    //     uint8_t pos = 0;
    //     char *pch = strchr(tmp_str, ',');
    //     int _s = 0;
    //     while (pch != NULL && pos < 13)
    //     {
    //         int len = pch - tmp_str + 1;
    //         pch = strchr(pch + 1, ',');
    //         memcpy(gps_info[pos], tmp_str + _s, len - _s - 1);
    //         _s = len;
    //         pos++;
    //     }
    //     if (pos > 4)
    //     {
    //         gpsParse(gps_info, pos);
    //         return;
    //     }
    //     return;
    // }

    // tmp_str = strstr(dt, "$GPGGA");
    // if (tmp_str)
    // {
    //     tmp_str = strtok(tmp_str, "\r\n");
    //     if (!tmp_str)
    //     {
    //         return;
    //     }

    //     char *check_str = strstr(tmp_str, ",,,,,");
    //     if (check_str != NULL)
    //     {
    //         return;
    //     }

    //     ESP_LOGI(TAG_GPS, "gps receive: %s", tmp_str);

    //     char gps_info[13][13] = {0};
    //     uint8_t pos = 0;
    //     char *pch = strchr(tmp_str, ',');
    //     int _s = 0;
    //     while (pch != NULL && pos < 13)
    //     {
    //         int len = pch - tmp_str + 1;
    //         pch = strchr(pch + 1, ',');
    //         memcpy(gps_info[pos], tmp_str + _s, len - _s - 1);
    //         _s = len;
    //         pos++;
    //     }

    //     if (pos > 7)
    //     {
    //         char *token;
    //         token = strtok(gps_info[1], ".");
    //         if (!token)
    //         {
    //             return;
    //         }
    //         float lat_temp = 0;
    //         float lon_temp = 0;
    //         if (!memcmp(gps_info[3], "N", 2))
    //         {
    //             lat_temp = parseLatLon(gps_info[2]);
    //         }
    //         else if (!memcmp(gps_info[3], "S", 2))
    //         {
    //             lat_temp = -parseLatLon(gps_info[2]);
    //         }
    //         else
    //         {
    //             return;
    //         }

    //         if (!memcmp(gps_info[5], "E", 2))
    //         {
    //             lon_temp = parseLatLon(gps_info[4]);
    //         }
    //         else if (!memcmp(gps_info[5], "W", 2))
    //         {
    //             lon_temp = -parseLatLon(gps_info[4]);
    //         }
    //         else
    //         {
    //             return;
    //         }
    //         float hdop = strtod(gps_info[8], NULL);
    //         ESP_LOGI(TAG_GPS, "GPGGA HDOP level precision factor: %f", hdop);
    //         if (hdop < hdop_min)
    //         {
    //             hdop_min = hdop;
    //             gps_.latitude = lat_temp;
    //             gps_.longitude = lon_temp;
    //             gps_.altitude = strtod(gps_info[9], NULL);
    //             char date[8] = {"dd:mm:yy"}; // default
    //             memcpy(gps_.time, date, 8);
    //             gps_.time[8] = ' ';
    //             ESP_LOGI(TAG_GPS, "GPGGA Datetime: %s Lat :%f Long: %f Altitude: %f", gps_.time, gps_.latitude, gps_.longitude, gps_.altitude);
    //         }
    //         // altitude - mean-sea-level (geoid) inmeters
    //     }
    //     return;
    // }

    tmp_str = strstr(dt, "$GPSACP");
    if (tmp_str)
    {
        ESP_LOGI(TAG_GPS, "GPS receive %s", tmp_str);
        tmp_str = strtok(tmp_str, "\r\n");
        if (!tmp_str)
        {
            return;
        }
    }
}

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *)malloc(RD_BUF_SIZE);
    for (;;)
    {
        // Waiting for UART event.
        if (xQueueReceive(uart0_queue, (void *)&event, (portTickType)portMAX_DELAY))
        {
            bzero(dtmp, RD_BUF_SIZE);
            switch (event.type)
            {
            // Event of UART receving data
            /*We'd better handler data event fast, there would be much more data events than
            other types of events. If we take too much time on data event, the queue might
            be full.*/
            case UART_DATA:
                f_response_at_cmd = true;
                uart_read_bytes(EX_UART_NUM, dtmp, event.size, portMAX_DELAY);
                // ESP_LOGI(TAG_GPS, "START========================================================================");
                // ESP_LOGI(TAG_GPS, "[DATA EVT]: %s", (char *)dtmp);
                // uart_write_bytes(EX_UART_NUM, (const char *)dtmp, event.size);
                dataParse(dtmp, event.size);
                // ESP_LOGI(TAG_GPS, "END==========================================================================\n\n\n");
                break;
            // Event of HW FIFO overflow detected
            case UART_FIFO_OVF:
                ESP_LOGI(TAG, "hw fifo overflow");
                // If fifo overflow happened, you should consider adding flow control for your application.
                // The ISR has already reset the rx FIFO,
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                break;
            // Event of UART ring buffer full
            case UART_BUFFER_FULL:
                ESP_LOGI(TAG, "ring buffer full");
                // If buffer full happened, you should consider encreasing your buffer size
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                break;
            // Event of UART RX break detected
            case UART_BREAK:
                ESP_LOGI(TAG, "uart rx break");
                break;
            // Event of UART parity check error
            case UART_PARITY_ERR:
                ESP_LOGI(TAG, "uart parity error");
                break;
            // Event of UART frame error
            case UART_FRAME_ERR:
                ESP_LOGI(TAG, "uart frame error");
                break;
            // Others
            default:
                ESP_LOGI(TAG, "uart event type: %d", event.type);
                break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

size_t dataSize(const char *data)
{
    size_t length = 0;
    while (data[length++])
        ;
    return length - 1;
}

// Default time out
void send_command(char *command)
{
    if (dataSize(command))
    {
        char cmd[dataSize(command) + 2];
        memset(cmd, 0, dataSize(command) + 2);
        memcpy(cmd, command, dataSize(command));
        cmd[dataSize(command)] = '\r';
        cmd[dataSize(command) + 1] = '\n';
        ESP_LOGI("cmd-> ", "%s", command);
        uart_write_bytes(EX_UART_NUM, cmd, dataSize(cmd));
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    ESP_LOGI(TAG, "====================================");
    esp_log_level_set(TAG, ESP_LOG_INFO);

    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    // Install UART driver, and get the queue.
    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);

    // Set UART pins (using UART0 default pins ie no changes.)
    uart_set_pin(EX_UART_NUM, TXD, RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // Set uart pattern detect function.
    uart_enable_pattern_det_baud_intr(EX_UART_NUM, '+', PATTERN_CHR_NUM, 9, 0, 0);
    // Reset the pattern queue length to record at most 20 pattern positions.
    uart_pattern_queue_reset(EX_UART_NUM, 20);

    gpio_config_t io_conf;
    // disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
    gpio_config(&io_conf);

    gpio_set_level(GPIO_OUTPUT_IO_0, true);
    gpio_set_direction(GPIO_OUTPUT_IO_1, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_OUTPUT_IO_1, true);

    // Create a task to handler UART event from ISR
    xTaskCreate(uart_event_task, "uart_event_task", 2048 * 2, NULL, 12, NULL);

    vTaskDelay(3000 / portTICK_PERIOD_MS);

    int _gps_search = 0;
    while (!f_response_at_cmd && _gps_search <= 20)
    {
        ESP_LOGI(TAG, "Searching gps...%d", _gps_search);
        send_command("AT");
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        _gps_search++;
    }
    send_command("AT+CMEE=2");
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    send_command("AT$GPSCFG?");
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    send_command("AT$GPSCFG=0,0");
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    send_command("AT$GPSCFG=2,1");
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    send_command("AT$GPSCFG=3,0");
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    send_command("AT$GPSP=1");
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    send_command("AT$GPSNMUNEX=0,1,1,0,0,0,0,0,0,0,0,1,0");
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    // AT$GPSNMUN=<enable>[,<GGA>,<GLL>,<GSA>,<GSV>,<RMC>,<VTG>]
    send_command("AT$GPSNMUN=2,1,0,1,1,1,0");
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    int _gps_call = 0;
    while (1)
    {
        send_command("AT$GPSACP");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        _gps_call++;
    }
}
