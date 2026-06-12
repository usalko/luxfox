#include "ulama/input_joystick.h"

#include <string.h>

void ulama_joystick_init_state(ulama_joystick_state_t *state)
{
	if (state == NULL) {
		return;
	}
	memset(state, 0, sizeof(*state));
}

void ulama_joystick_apply_event(struct js_event event,
	uint16_t channels[ULAMA_CRSF_NUM_CHANNELS],
	ulama_joystick_state_t *state)
{
	if (channels == NULL || state == NULL) {
		return;
	}

	event.type &= (uint8_t)~JS_EVENT_INIT;
	if (event.type == JS_EVENT_AXIS) {
		switch (event.number) {
		case 0:
			channels[0] = ulama_crsf_axis_to_bipolar(event.value, false);
			break;
		case 1:
			channels[1] = ulama_crsf_axis_to_bipolar(event.value, true);
			break;
		case 2:
			channels[2] = ulama_crsf_axis_to_throttle(event.value, true);
			break;
		case 3:
			channels[3] = ulama_crsf_axis_to_bipolar(event.value, false);
			break;
		default:
			break;
		}
		return;
	}

	if (event.type == JS_EVENT_BUTTON && event.number < ULAMA_CRSF_NUM_CHANNELS) {
		if (event.number == 0 && event.value != 0 && state->button_state[0] == 0) {
			state->arm_state = !state->arm_state;
			channels[4] = state->arm_state ? ULAMA_CRSF_VALUE_MAX : ULAMA_CRSF_VALUE_MIN;
		}
		state->button_state[event.number] = event.value;
		if (event.number > 0 && (size_t)(4 + event.number) < ULAMA_CRSF_NUM_CHANNELS) {
			channels[4 + event.number] = event.value != 0 ? ULAMA_CRSF_VALUE_MAX : ULAMA_CRSF_VALUE_MIN;
		}
	}
}