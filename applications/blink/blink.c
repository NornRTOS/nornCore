/**
 * @file blink.c
 * The smallest running nornOS application: one task that toggles the on-board LED every
 * 500 ms (the blue LED on GPIO2 of the ESP32 devkit, pin 13 on the AVR boards) and the
 * idle task that sleeps in between. No UART, no other kernel objects; its size is the
 * floor of what a nornOS image costs.
 */

#include "rtos.h"
#include "board.h"

#define TASK_BLINK      1u
#define BLINK_MS        500u

static const gpio_pin_t _pinLed = BOARD_PIN_LED;
static uint8_t _stackBlink[2u * RTOS_PORT_MIN_STACK];


static void taskBlink(void *arg)
{
    (void)arg;

    gpio_output(&_pinLed);
    for(;;)
    {
        gpio_toggle(&_pinLed);
        rtos_delay(RTOS_MS(BLINK_MS));
    }
}


int main(void)
{
    rtos_init();
    rtos_createTask(TASK_BLINK, taskBlink, NULL, _stackBlink, sizeof(_stackBlink));
    rtos_start();
    return 0;
}
