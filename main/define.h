#ifndef __DEFINE_H__
#define __DEFINE_H__

#include <hal/gpio_types.h>
#include <define.h>

#define EX_UART_NUM UART_NUM_2

#define BUF_SIZE_1024 (1024)
#define RD_BUF_SIZE (BUF_SIZE_1024)

#define TXD (GPIO_NUM_16)
#define RXD (GPIO_NUM_17)

#define GPIO_OUTPUT_IO_0 25
#define GPIO_OUTPUT_IO_1 22
#define GPIO_OUTPUT_IO_2 27

#define PATTERN_CHR_NUM (3) /*!< Set the number of consecutive and identical characters received by receiver which defines a UART pattern*/
#define GPIO_OUTPUT_PIN_SEL (1ULL << GPIO_OUTPUT_IO_0 | 1ULL << GPIO_OUTPUT_IO_1 | 1ULL << GPIO_OUTPUT_IO_2)

#define ON true
#define OFF false

#endif
