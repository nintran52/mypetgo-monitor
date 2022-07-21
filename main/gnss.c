#include <gnss.h>
#include <define.h>
#include <driver/gpio.h>
#include "esp_log.h"

static QueueHandle_t uart0_queue;
#define RESPONSE_TIMEOUT_MS (30000)

static const char *TAG = "GNSS";

void me310_power(bool value)
{
    gpio_set_level(GPIO_OUTPUT_IO_0, value);
}

void me310_init()
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    // Install UART driver, and get the queue.
    uart_driver_install(EX_UART_NUM, BUF_SIZE_1024 * 2, BUF_SIZE_1024 * 2, 20, &uart0_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);

    // Set UART pins (using UART0 default pins ie no changes.)
    uart_set_pin(EX_UART_NUM, TXD, RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // Set uart pattern detect function.
    uart_enable_pattern_det_baud_intr(EX_UART_NUM, '+', PATTERN_CHR_NUM, 9, 0, 0);
    // Reset the pattern queue length to record at most 20 pattern positions.
    uart_pattern_queue_reset(EX_UART_NUM, 20);

    // gpio_config_t io_conf;
    // // disable interrupt
    // io_conf.intr_type = GPIO_INTR_DISABLE;
    // // set as output mode
    // io_conf.mode = GPIO_MODE_OUTPUT;
    // // bit mask of the pins that you want to set,e.g.GPIO18/19
    // io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    // // disable pull-down mode
    // io_conf.pull_down_en = 0;
    // // disable pull-up mode
    // io_conf.pull_up_en = 0;
    // // configure GPIO with the given settings
    // gpio_config(&io_conf);

    // me310_power(ON);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Inited UART driver");
}