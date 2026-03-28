#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <soc.h>

LOG_MODULE_REGISTER(yolochka_static_led_strip, CONFIG_LOG_DEFAULT_LEVEL);

#define AUX_LED_PIN 8U
#define AUX_LED_PIN_MASK BIT(AUX_LED_PIN)

#define AUX_LED_COUNT 5
#define AUX_LED_STARTUP_DELAY_MS 1000
#define AUX_LED_REFRESH_MS 1000
#define AUX_LED_RESET_US 80

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

/*
 * Timing-sensitive GPIO bitbang for SK6812/WS2812 on nRF52840 @ 64 MHz.
 * If colors are still wrong, the first thing to tune is the NOP counts below.
 */
#define AUX_LED_SET_HIGH "str %[pin], [%[base], #0]\n"
#define AUX_LED_SET_LOW "str %[pin], [%[base], #4]\n"

#define AUX_LED_DELAY_T1H                                                                            \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"                                                     \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"                                                     \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"                                                     \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"

#define AUX_LED_DELAY_T0H                                                                            \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"                                                     \
    "nop\nnop\nnop\nnop\n"

#define AUX_LED_DELAY_TXL                                                                            \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"                                                     \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"                                                     \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"

#define AUX_LED_ONE_BIT(base, pin)                                                                  \
    do {                                                                                            \
        __asm__ volatile(AUX_LED_SET_HIGH AUX_LED_DELAY_T1H AUX_LED_SET_LOW AUX_LED_DELAY_TXL      \
                         :                                                                          \
                         : [base] "l"(base), [pin] "l"(pin)                                        \
                         : "memory");                                                              \
    } while (false)

#define AUX_LED_ZERO_BIT(base, pin)                                                                 \
    do {                                                                                            \
        __asm__ volatile(AUX_LED_SET_HIGH AUX_LED_DELAY_T0H AUX_LED_SET_LOW AUX_LED_DELAY_TXL      \
                         :                                                                          \
                         : [base] "l"(base), [pin] "l"(pin)                                        \
                         : "memory");                                                              \
    } while (false)

static void aux_led_send_byte(uint8_t value) {
    volatile uint32_t *base = (volatile uint32_t *)&NRF_P0->OUTSET;

    for (int bit = 7; bit >= 0; bit--) {
        if (value & BIT(bit)) {
            AUX_LED_ONE_BIT(base, AUX_LED_PIN_MASK);
        } else {
            AUX_LED_ZERO_BIT(base, AUX_LED_PIN_MASK);
        }
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
    NRF_P0->DIRSET = AUX_LED_PIN_MASK;
    NRF_P0->OUTCLR = AUX_LED_PIN_MASK;

    k_work_init_delayable(&aux_led_start_work, yolochka_static_led_strip_start);
    k_work_schedule(&aux_led_start_work, K_MSEC(AUX_LED_STARTUP_DELAY_MS));
    LOG_INF("Scheduled GPIO bitbang updates for %u pixels on P0.08", AUX_LED_COUNT);

    return 0;
}

SYS_INIT(yolochka_static_led_strip_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
