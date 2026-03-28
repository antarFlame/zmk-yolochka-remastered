#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_gpio.h>
#include <nrfx_pwm.h>

LOG_MODULE_REGISTER(yolochka_static_led_strip, CONFIG_LOG_DEFAULT_LEVEL);

#define AUX_LED_PIN NRF_GPIO_PIN_MAP(0, 8)

#define AUX_LED_COUNT 5
#define AUX_LED_STARTUP_DELAY_MS 1000
#define AUX_LED_REFRESH_MS 1000

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
 *    aux_led_encode_pixel(). Many WS2812/SK6812 strips are GRB, but not all.
 */
#define AUX_LED_BRIGHTNESS 128
#define AUX_LED_BASE_RED 255
#define AUX_LED_BASE_GREEN 0
#define AUX_LED_BASE_BLUE 0

/*
 * PWM timings for WS2812/SK6812-like LEDs.
 *
 * We encode one LED data bit as one PWM period:
 * - 16 MHz base clock
 * - 20 ticks per period = 1.25 us
 * - duty  5/20 ~= 0.312 us for logical 0
 * - duty 10/20 ~= 0.625 us for logical 1
 *
 * This is conceptually closer to libraries like LiteLED that rely on
 * hardware-timed symbol generation instead of CPU busy-wait loops.
 *
 * For nRF PWM sequence values, bit 15 controls output polarity. WS2812 PWM
 * implementations on nRF set this bit so the line returns low after each
 * symbol and stays low during the reset tail.
 */
#define AUX_LED_PWM_TOP 20U
#define AUX_LED_PWM_T0H 5U
#define AUX_LED_PWM_T1H 10U
#define AUX_LED_PWM_POLARITY BIT(15)
#define AUX_LED_RESET_SLOTS 128U

#define AUX_LED_BITS_PER_PIXEL 24U
#define AUX_LED_FRAME_SLOTS ((AUX_LED_COUNT * AUX_LED_BITS_PER_PIXEL) + AUX_LED_RESET_SLOTS)

static struct k_work_delayable aux_led_start_work;
static uint32_t aux_led_attempt;
static bool aux_led_pwm_ready;

static nrfx_pwm_t aux_led_pwm = NRFX_PWM_INSTANCE(0);
static nrf_pwm_values_common_t aux_led_pwm_values[AUX_LED_FRAME_SLOTS];
static nrf_pwm_sequence_t aux_led_pwm_sequence = {
    .values = {.p_common = aux_led_pwm_values},
    .length = ARRAY_SIZE(aux_led_pwm_values),
    .repeats = 0,
    .end_delay = 0,
};

static inline uint8_t aux_led_scale(uint8_t value) {
    return (uint8_t)(((uint16_t)value * AUX_LED_BRIGHTNESS) / 255U);
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

static size_t aux_led_encode_byte(size_t slot, uint8_t value) {
    for (int bit = 7; bit >= 0; bit--) {
        aux_led_pwm_values[slot++] =
            ((value & BIT(bit)) ? AUX_LED_PWM_T1H : AUX_LED_PWM_T0H) | AUX_LED_PWM_POLARITY;
    }

    return slot;
}

static size_t aux_led_encode_pixel(size_t slot, uint8_t red, uint8_t green, uint8_t blue) {
    /*
     * Current byte order is GRB.
     * If "red" comes out as another color on your strip, swap the order here.
     */
    slot = aux_led_encode_byte(slot, green);
    slot = aux_led_encode_byte(slot, red);
    slot = aux_led_encode_byte(slot, blue);

    return slot;
}

static void aux_led_prepare_frame(void) {
    size_t slot = 0;

    for (int i = 0; i < AUX_LED_COUNT; i++) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        aux_led_get_color_for_index(i, &red, &green, &blue);
        slot = aux_led_encode_pixel(slot, red, green, blue);
    }

    while (slot < ARRAY_SIZE(aux_led_pwm_values)) {
        aux_led_pwm_values[slot++] = AUX_LED_PWM_POLARITY;
    }
}

static int aux_led_pwm_init_once(void) {
    nrfx_pwm_config_t config = NRFX_PWM_DEFAULT_CONFIG(
        AUX_LED_PIN, NRF_PWM_PIN_NOT_CONNECTED, NRF_PWM_PIN_NOT_CONNECTED,
        NRF_PWM_PIN_NOT_CONNECTED);

    config.base_clock = NRF_PWM_CLK_16MHz;
    config.top_value = AUX_LED_PWM_TOP;
    config.load_mode = NRF_PWM_LOAD_COMMON;
    config.step_mode = NRF_PWM_STEP_AUTO;

    nrfx_err_t err = nrfx_pwm_init(&aux_led_pwm, &config, NULL, NULL);
    if ((err != NRFX_SUCCESS) && (err != NRFX_ERROR_INVALID_STATE)) {
        LOG_ERR("nrfx_pwm_init failed: %d", err);
        return -EIO;
    }

    aux_led_pwm_ready = true;
    return 0;
}

static void aux_led_send_frame(void) {
    if (!aux_led_pwm_ready) {
        return;
    }

    aux_led_prepare_frame();

    nrfx_pwm_simple_playback(&aux_led_pwm, &aux_led_pwm_sequence, 1,
                             NRFX_PWM_FLAG_STOP | NRFX_PWM_FLAG_NO_EVT_FINISHED);

    while (!nrfx_pwm_stopped_check(&aux_led_pwm)) {
    }
}

static void yolochka_static_led_strip_start(struct k_work *work) {
    ARG_UNUSED(work);
    aux_led_attempt++;

    aux_led_send_frame();
    LOG_INF("Attempt %u: PWM-updated %u pixels on P0.08", aux_led_attempt, AUX_LED_COUNT);

    k_work_schedule(&aux_led_start_work, K_MSEC(AUX_LED_REFRESH_MS));
}

static int yolochka_static_led_strip_init(void) {
    int err = aux_led_pwm_init_once();
    if (err != 0) {
        return err;
    }

    k_work_init_delayable(&aux_led_start_work, yolochka_static_led_strip_start);
    k_work_schedule(&aux_led_start_work, K_MSEC(AUX_LED_STARTUP_DELAY_MS));
    LOG_INF("Scheduled PWM updates for %u pixels on P0.08", AUX_LED_COUNT);

    return 0;
}

SYS_INIT(yolochka_static_led_strip_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
