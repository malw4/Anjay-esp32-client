#ifndef LED_COLOR_LIGHT_H
#define LED_COLOR_LIGHT_H

#include <anjay/anjay.h>

#define RGB_VALUE_STR_BUFLEN sizeof("#RRGGBB")
#define RGB_COLOR_COUNT 3

extern uint8_t led_strip_state[RGB_COLOR_COUNT];

#endif // LED_COLOR_LIGHT_H
