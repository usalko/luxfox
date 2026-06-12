#pragma once

#include <linux/joystick.h>
#include <stdbool.h>
#include <stdint.h>

#include "ulama/crsf.h"

typedef struct {
	int button_state[ULAMA_CRSF_NUM_CHANNELS];
	bool arm_state;
} ulama_joystick_state_t;

void ulama_joystick_init_state(ulama_joystick_state_t *state);
void ulama_joystick_apply_event(struct js_event event,
	uint16_t channels[ULAMA_CRSF_NUM_CHANNELS],
	ulama_joystick_state_t *state);