#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(yolochka_static_led_strip, CONFIG_LOG_DEFAULT_LEVEL);

#define AUX_LED_GPIO_NODE DT_NODELABEL(static_led_gpio)

#if DT_NODE_EXISTS(AUX_LED_GPIO_NODE)

static const struct gpio_dt_spec aux_led_gpio = GPIO_DT_SPEC_GET(AUX_LED_GPIO_NODE, gpios);

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
 * A fixed red glow for the extra strip on P0.08.
 * 128 is about 50% of the 8-bit channel range.
 */
#define AUX_LED_RED 128
#define AUX_LED_GREEN 0
#define AUX_LED_BLUE 0

static struct k_work_delayable aux_led_start_work;
static uint32_t aux_led_attempt;

static inline void aux_led_wait_until(uint32_t start_cycles, uint32_t delta_cycles) {
    while ((uint32_t)(k_cycle_get_32() - start_cycles) < delta_cycles) {
    }
}

static inline void aux_led_send_bit(bool one) {
    uint32_t start_cycles = k_cycle_get_32();

    nrf_gpio_pin_set(aux_led_gpio.pin);
    aux_led_wait_until(start_cycles, one ? AUX_LED_T1H_CYCLES : AUX_LED_T0H_CYCLES);
    nrf_gpio_pin_clear(aux_led_gpio.pin);
    aux_led_wait_until(start_cycles, AUX_LED_PERIOD_CYCLES);
}

static void aux_led_send_byte(uint8_t value) {
    for (int bit = 7; bit >= 0; bit--) {
        aux_led_send_bit((value & BIT(bit)) != 0U);
    }
}

static void aux_led_send_frame(void) {
    unsigned int key = irq_lock();

    for (int i = 0; i < AUX_LED_COUNT; i++) {
        aux_led_send_byte(AUX_LED_GREEN);
        aux_led_send_byte(AUX_LED_RED);
        aux_led_send_byte(AUX_LED_BLUE);
    }

    irq_unlock(key);
    k_busy_wait(AUX_LED_RESET_US);
}

static void yolochka_static_led_strip_start(struct k_work *work) {
    ARG_UNUSED(work);
    aux_led_attempt++;

    if (!device_is_ready(aux_led_gpio.port)) {
        LOG_ERR("Attempt %u: GPIO controller for P0.08 is not ready", aux_led_attempt);
        return;
    }

    aux_led_send_frame();
    LOG_INF("Attempt %u: bit-banged %u pixels on P0.08", aux_led_attempt, AUX_LED_COUNT);

    k_work_schedule(&aux_led_start_work, K_MSEC(AUX_LED_REFRESH_MS));
}

static int yolochka_static_led_strip_init(void) {
    if (!device_is_ready(aux_led_gpio.port)) {
        LOG_ERR("GPIO controller for P0.08 is not ready");
        return -ENODEV;
    }

    if (gpio_pin_configure_dt(&aux_led_gpio, GPIO_OUTPUT_INACTIVE) < 0) {
        LOG_ERR("Failed to configure P0.08 for static LED output");
        return -EIO;
    }

    k_work_init_delayable(&aux_led_start_work, yolochka_static_led_strip_start);
    k_work_schedule(&aux_led_start_work, K_MSEC(AUX_LED_STARTUP_DELAY_MS));
    LOG_INF("Scheduled GPIO bitbang updates for %u pixels on P0.08", AUX_LED_COUNT);

    return 0;
}

SYS_INIT(yolochka_static_led_strip_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
