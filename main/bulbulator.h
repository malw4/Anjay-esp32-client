#ifndef BULBULATOR_H
#define BULBULATOR_H
typedef enum {
    BULBULATOR_IDLE,
    BULBULATOR_START,
    BULBULATOR_READY,
    BULBULATOR_SET,
    BULBULATOR_GO,
    BULBULATOR_MEASURE,
    BULBULATOR_END
} bulbulator_state_t;

extern bulbulator_state_t bulb_state;

#define FLOW_PIN 13
#endif // BULBULATOR_H
