#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(yolochka_static_led_strip, CONFIG_LOG_DEFAULT_LEVEL);

#define AUX_LED_PIN NRF_GPIO_PIN_MAP(0, 8)

#define AUX_LED_COUNT 5
#define AUX_LED_STARTUP_DELAY_MS 1000
#define AUX_LED_REFRESH_MS 1000

#define AUX_LED_PERIOD_NS 1250
#define AUX_LED_T0H_NS 350
#define AUX_LED_T1H_NS 700
#define AUX_LED_RESET_US 80

#define NS_TO_CYCLES(ns)                                                                           \
    ((uint32_t)((((uint64_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC) * (ns)) / 1000000000ULL))

#define AUX_LED_PERIOD_CYCLES NS_TO_CYCLES(AUX_LED_PERIOD_NS)
#define AUX_LED_T0H_CYCLES NS_TO_CYCLES(AUX_LED_T0H_NS)
#define AUX_LED_T1H_CYCLES NS_TO_CYCLES(AUX_LED_T1H_NS)

/*
 * User-tunable color setup.
 *
 * 1. Change AUX_LED_BRIGHTNESS to set overall brightness.
 *    255 = max, 128 ~= 50%, 32 = dim.
 * 2. Change AUX_LED_BASE_RED/GREEN/BLUE to set the base color.
 *    Example red:   255,   0,   0
 *    Example green:   0, 255,   0
 *    Example white: 255, 255, 255
 * 3. If the strip shows the wrong color, adjust the byte order in
 *    aux_led_send_pixel(). Many WS2812/SK6812 strips are GRB, but not all.
 */
#define AUX_LED_BRIGHTNESS 128
#define AUX_LED_BASE_RED 255
#define AUX_LED_BASE_GREEN 0
#define AUX_LED_BASE_BLUE 0

static struct k_work_delayable aux_led_start_work;
static uint32_t aux_led_attempt;

static inline uint8_t aux_led_scale(uint8_t value) {
    return (uint8_t)(((uint16_t)value * AUX_LED_BRIGHTNESS) / 255U);
}

static inline void aux_led_wait_until(uint32_t start_cycles, uint32_t delta_cycles) {
    while ((uint32_t)(k_cycle_get_32() - start_cycles) < delta_cycles) {
    }
}

static inline void aux_led_send_bit(bool one) {
    uint32_t start_cycles = k_cycle_get_32();

    nrf_gpio_pin_set(AUX_LED_PIN);
    aux_led_wait_until(start_cycles, one ? AUX_LED_T1H_CYCLES : AUX_LED_T0H_CYCLES);
    nrf_gpio_pin_clear(AUX_LED_PIN);
    aux_led_wait_until(start_cycles, AUX_LED_PERIOD_CYCLES);
}

static void aux_led_send_byte(uint8_t value) {
    for (int bit = 7; bit >= 0; bit--) {
        aux_led_send_bit((value & BIT(bit)) != 0U);
    }
}

static void aux_led_send_pixel(uint8_t red, uint8_t green, uint8_t blue) {
    /*
     * Current byte order is GRB.
     * If "red" comes out as another color on your strip, swap the order here.
     */
    aux_led_send_byte(green);
    aux_led_send_byte(red);
    aux_led_send_byte(blue);
}

static void aux_led_get_color_for_index(int led_index, uint8_t *red, uint8_t *green,
                                        uint8_t *blue) {
    ARG_UNUSED(led_index);

    /*
     * This is the color assignment algorithm for the strip.
     * Right now all LEDs use one shared static color.
     *
     * If you want a gradient or per-LED colors, change this function.
     * Example:
     * if (led_index == 0) { *red = 255; *green = 0; *blue = 0; }
     * else { *red = 0; *green = 0; *blue = 255; }
     */
    *red = aux_led_scale(AUX_LED_BASE_RED);
    *green = aux_led_scale(AUX_LED_BASE_GREEN);
    *blue = aux_led_scale(AUX_LED_BASE_BLUE);
}

static void aux_led_send_frame(void) {
    unsigned int key = irq_lock();

    for (int i = 0; i < AUX_LED_COUNT; i++) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        aux_led_get_color_for_index(i, &red, &green, &blue);
        aux_led_send_pixel(red, green, blue);
    }

    irq_unlock(key);
    k_busy_wait(AUX_LED_RESET_US);
}

static void yolochka_static_led_strip_start(struct k_work *work) {
    ARG_UNUSED(work);
    aux_led_attempt++;

    aux_led_send_frame();
    LOG_INF("Attempt %u: bit-banged %u pixels on P0.08", aux_led_attempt, AUX_LED_COUNT);

    k_work_schedule(&aux_led_start_work, K_MSEC(AUX_LED_REFRESH_MS));
}

static int yolochka_static_led_strip_init(void) {
    nrf_gpio_cfg_output(AUX_LED_PIN);
    nrf_gpio_pin_clear(AUX_LED_PIN);

    k_work_init_delayable(&aux_led_start_work, yolochka_static_led_strip_start);
    k_work_schedule(&aux_led_start_work, K_MSEC(AUX_LED_STARTUP_DELAY_MS));
    LOG_INF("Scheduled GPIO bitbang updates for %u pixels on P0.08", AUX_LED_COUNT);

    return 0;
}

SYS_INIT(yolochka_static_led_strip_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
