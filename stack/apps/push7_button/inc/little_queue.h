#ifndef __LITTLE_QUEUE_H
#define __LITTLE_QUEUE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QUEUE_ELEMENTS 20

typedef enum {
    LOW_PRIORITY = 0,
    NORMAL_PRIORITY = 1,
    TOP_PRIORITY = 2,
} queue_priority_t;

void little_queue_init();
void queue_add_file(uint8_t* file_content, uint8_t file_size, uint8_t file_id);
void little_queue_toggle_led_state(bool state);
bool little_queue_get_toggle_led_state();

#endif //__LITTLE_QUEUE_H