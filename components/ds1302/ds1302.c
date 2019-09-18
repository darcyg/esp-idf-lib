/**
 * @file ds1302.h
 *
 * ESP-IDF driver for DS1302 RTC
 *
 * Ported from esp-open-rtos
 *
 * Copyright (C) 2016, 2019 Ruslan V. Uss <unclerus@gmail.com>\n
 * Copyright (C) 2016 Pavel Merzlyakov <merzlyakovpavel@gmail.com>
 *
 * BSD Licensed as described in the file LICENSE
 */
#include "ds1302.h"
#include <freertos/FreeRTOS.h>
#include <string.h>
#if defined(CONFIG_IDF_TARGET_ESP32)
#include <esp32/rom/ets_sys.h>
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(PROJECT_CONFIG_IDF_TARGET_ESP32)
#include <rom/ets_sys.h>
#else
#error cannot locate ets_sys.h
#endif

#define CH_REG   0x80
#define WP_REG   0x8e

#define CH_BIT     (1 << 7)
#define WP_BIT     (1 << 7)
#define HOUR12_BIT (1 << 7)
#define PM_BIT     (1 << 5)

#define CH_MASK ((uint8_t)(~CH_BIT))
#define WP_MASK ((uint8_t)(~WP_BIT))

#define CLOCK_BURST 0xbe
#define RAM_BURST   0xfe

#define SECONDS_MASK 0x7f
#define HOUR12_MASK  0x1f
#define HOUR24_MASK  0x3f

#define GPIO_BIT(x) (1ULL << (x))

#define CHECK(x) do { esp_err_t __; if ((__ = x) != ESP_OK) return __; } while (0)
#define CHECK_ARG(VAL) do { if (!(VAL)) return ESP_ERR_INVALID_ARG; } while (0)

#if defined(CONFIG_IDF_TARGET_ESP32) || defined(PROJECT_CONFIG_IDF_TARGET_ESP32)
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#define PORT_ENTER_CRITICAL portENTER_CRITICAL(&mux)
#define PORT_EXIT_CRITICAL portEXIT_CRITICAL(&mux)

#elif defined(CONFIG_IDF_TARGET_ESP8266)
#define PORT_ENTER_CRITICAL portENTER_CRITICAL()
#define PORT_EXIT_CRITICAL portEXIT_CRITICAL()
#endif

#define CHECK_MUX(x) do { esp_err_t __; if ((__ = (x)) != ESP_OK) { PORT_EXIT_CRITICAL; return __; } } while (0)

static uint8_t bcd2dec(uint8_t val)
{
    return (val >> 4) * 10 + (val & 0x0f);
}

static uint8_t dec2bcd(uint8_t val)
{
    return ((val / 10) << 4) + (val % 10);
}

inline static esp_err_t chip_enable(ds1302_t *dev)
{
    CHECK(gpio_set_level(dev->ce_pin, 1));
    ets_delay_us(4);
    return ESP_OK;
}

inline static esp_err_t chip_disable(ds1302_t *dev)
{
    return gpio_set_level(dev->ce_pin, 0);
}

inline static esp_err_t prepare(ds1302_t *dev, gpio_mode_t mode)
{
    CHECK(gpio_set_direction(dev->io_pin, mode));
    CHECK(gpio_set_level(dev->sclk_pin, 0));
    return chip_enable(dev);
}

inline static esp_err_t toggle_clock(ds1302_t *dev)
{
    CHECK(gpio_set_level(dev->sclk_pin, 1));
    ets_delay_us(1);
    CHECK(gpio_set_level(dev->sclk_pin, 0));
    ets_delay_us(1);
    return ESP_OK;
}

static esp_err_t write_byte(ds1302_t *dev, uint8_t b)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        CHECK(gpio_set_level(dev->io_pin, (b >> i) & 1));
        CHECK(toggle_clock(dev));
    }
    return ESP_OK;
}

static esp_err_t read_byte(ds1302_t *dev, uint8_t *b)
{
    *b = 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        *b |= gpio_get_level(dev->io_pin) << i;
        CHECK(toggle_clock(dev));
    }
    return ESP_OK;
}

static esp_err_t read_register(ds1302_t *dev, uint8_t reg, uint8_t *val)
{
    PORT_ENTER_CRITICAL;
    CHECK_MUX(prepare(dev, GPIO_MODE_OUTPUT));
    CHECK_MUX(write_byte(dev, reg | 0x01));
    CHECK_MUX(prepare(dev, GPIO_MODE_INPUT));
    CHECK_MUX(read_byte(dev, val));
    PORT_EXIT_CRITICAL;
    return chip_disable(dev);
}

static esp_err_t write_register(ds1302_t *dev, uint8_t reg, uint8_t val)
{
    PORT_ENTER_CRITICAL;
    CHECK_MUX(prepare(dev, GPIO_MODE_OUTPUT));
    CHECK_MUX(write_byte(dev, reg));
    CHECK_MUX(write_byte(dev, val));
    PORT_ENTER_CRITICAL;
    return chip_disable(dev);
}

static esp_err_t burst_read(ds1302_t *dev, uint8_t reg, uint8_t *dst, uint8_t len)
{
    PORT_ENTER_CRITICAL;
    CHECK_MUX(prepare(dev, GPIO_MODE_OUTPUT));
    CHECK_MUX(write_byte(dev, reg | 0x01));
    CHECK_MUX(prepare(dev, GPIO_MODE_INPUT));
    for (uint8_t i = 0; i < len; i++, dst++)
        CHECK_MUX(read_byte(dev, dst));
    PORT_EXIT_CRITICAL;
    return chip_disable(dev);
}

static esp_err_t burst_write(ds1302_t *dev, uint8_t reg, uint8_t *src, uint8_t len)
{
    PORT_ENTER_CRITICAL;
    CHECK_MUX(prepare(dev, GPIO_MODE_OUTPUT));
    CHECK_MUX(write_byte(dev, reg));
    for (uint8_t i = 0; i < len; i++, src++)
        CHECK_MUX(write_byte(dev, *src));
    PORT_EXIT_CRITICAL;
    return chip_disable(dev);
}

static esp_err_t update_register(ds1302_t *dev, uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t r;
    CHECK(read_register(dev, reg, &r));
    return write_register(dev, reg, (r & mask) | val);
}

///////////////////////////////////////////////////////////////////////////////

esp_err_t ds1302_init(ds1302_t *dev)
{
    CHECK_ARG(dev);

    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(gpio_config_t));
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask =
            GPIO_BIT(dev->ce_pin) |
            GPIO_BIT(dev->io_pin) |
            GPIO_BIT(dev->sclk_pin);
    CHECK(gpio_config(&io_conf));

    bool r;
    CHECK(ds1302_is_running(dev, &r));
    dev->ch = !r;

    return ESP_OK;
}

esp_err_t ds1302_start(ds1302_t *dev, bool start)
{
    CHECK_ARG(dev);

    CHECK(update_register(dev, CH_REG, CH_MASK, start ? 0 : CH_BIT));
    dev->ch = !start;

    return ESP_OK;
}

esp_err_t ds1302_is_running(ds1302_t *dev, bool *running)
{
    CHECK_ARG(dev);
    CHECK_ARG(running);

    uint8_t r;
    CHECK(read_register(dev, CH_REG, &r));
    *running = !(r & CH_BIT);
    dev->ch = !*running;

    return ESP_OK;
}

esp_err_t ds1302_set_write_protect(ds1302_t *dev, bool wp)
{
    CHECK_ARG(dev);

    return update_register(dev, WP_REG, WP_MASK, wp ? WP_BIT : 0);
}

esp_err_t ds1302_get_write_protect(ds1302_t *dev, bool *wp)
{
    CHECK_ARG(dev);
    CHECK_ARG(wp);

    uint8_t r;
    CHECK(read_register(dev, WP_REG, &r));
    *wp = (r & WP_BIT) != 0;

    return ESP_OK;
}

esp_err_t ds1302_get_time(ds1302_t *dev, struct tm *time)
{
    CHECK_ARG(dev);
    CHECK_ARG(time);

    uint8_t buf[7];
    CHECK(burst_read(dev, CLOCK_BURST, buf, 7));

    time->tm_sec = bcd2dec(buf[0] & SECONDS_MASK);
    time->tm_min = bcd2dec(buf[1]);
    if (buf[2] & HOUR12_BIT)
    {
        // RTC in 12-hour mode
        time->tm_hour = bcd2dec(buf[2] & HOUR12_MASK) - 1;
        if (buf[2] & PM_BIT)
            time->tm_hour += 12;
    }
    else time->tm_hour = bcd2dec(buf[2] & HOUR24_MASK);
    time->tm_mday = bcd2dec(buf[3]);
    time->tm_mon  = bcd2dec(buf[4]) - 1;
    time->tm_wday = bcd2dec(buf[5]) - 1;
    time->tm_year = bcd2dec(buf[6]) + 2000;

    return ESP_OK;
}

esp_err_t ds1302_set_time(ds1302_t *dev, const struct tm *time)
{
    CHECK_ARG(dev);
    CHECK_ARG(time);

    uint8_t buf[8] = {
        dec2bcd(time->tm_sec) | (dev->ch ? CH_BIT : 0),
        dec2bcd(time->tm_min),
        dec2bcd(time->tm_hour),
        dec2bcd(time->tm_mday),
        dec2bcd(time->tm_mon  + 1),
        dec2bcd(time->tm_wday + 1),
        dec2bcd(time->tm_year - 2000),
        0
    };
    return burst_write(dev, CLOCK_BURST, buf, 8);
}

esp_err_t ds1302_read_sram(ds1302_t *dev, uint8_t offset, void *buf, uint8_t len)
{
    CHECK_ARG(dev);
    CHECK_ARG(buf);
    CHECK_ARG(len);
    CHECK_ARG(offset + len <= DS1302_RAM_SIZE);

    return burst_read(dev, RAM_BURST, (uint8_t *)buf, len);
}

esp_err_t ds1302_write_sram(ds1302_t *dev, uint8_t offset, void *buf, uint8_t len)
{
    CHECK_ARG(dev);
    CHECK_ARG(buf);
    CHECK_ARG(len);
    CHECK_ARG(offset + len <= DS1302_RAM_SIZE);

    return burst_write(dev, RAM_BURST, (uint8_t *)buf, len);
}
