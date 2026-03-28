#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#define AUX_LED_STRIP_NODE DT_CHOSEN(yolochka_static_led_strip)

#if DT_NODE_EXISTS(AUX_LED_STRIP_NODE)

#define AUX_LED_COUNT DT_PROP(AUX_LED_STRIP_NODE, chain_length)

/*
 * A fixed red glow for the extra strip on P0.08.
 * 128 is about 50% of the 8-bit channel range.
 */
#define AUX_LED_RED 128
#define AUX_LED_GREEN 0
#define AUX_LED_BLUE 0

static int yolochka_static_led_strip_init(void) {
    const struct device *strip = DEVICE_DT_GET(AUX_LED_STRIP_NODE);
    struct led_rgb pixels[AUX_LED_COUNT];

    if (!device_is_ready(strip)) {
        return -ENODEV;
    }

    for (size_t i = 0; i < ARRAY_SIZE(pixels); i++) {
        pixels[i].r = AUX_LED_RED;
        pixels[i].g = AUX_LED_GREEN;
        pixels[i].b = AUX_LED_BLUE;
    }

    return led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));
}

SYS_INIT(yolochka_static_led_strip_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
