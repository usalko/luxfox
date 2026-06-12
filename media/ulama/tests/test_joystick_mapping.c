#include <linux/joystick.h>
#include <stdint.h>
#include <stdio.h>

#include "ulama/crsf.h"
#include "ulama/input_joystick.h"

static int expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int main(void)
{
	uint16_t channels[ULAMA_CRSF_NUM_CHANNELS];
	ulama_joystick_state_t state;
	int rc = 0;

	ulama_crsf_channels_init(channels);
	ulama_joystick_init_state(&state);

	ulama_joystick_apply_event((struct js_event){
		.type = JS_EVENT_AXIS,
		.number = 0,
		.value = 32767,
	}, channels, &state);
	rc |= expect_true(channels[0] == ulama_crsf_axis_to_bipolar(32767, false), "axis 0 should map to roll");

	ulama_joystick_apply_event((struct js_event){
		.type = JS_EVENT_AXIS | JS_EVENT_INIT,
		.number = 1,
		.value = -32767,
	}, channels, &state);
	rc |= expect_true(channels[1] == ulama_crsf_axis_to_bipolar(-32767, true), "axis 1 should map to pitch with init flag stripped");

	ulama_joystick_apply_event((struct js_event){
		.type = JS_EVENT_AXIS,
		.number = 2,
		.value = -32767,
	}, channels, &state);
	rc |= expect_true(channels[2] == ulama_crsf_axis_to_throttle(-32767, true), "axis 2 should map to throttle");

	ulama_joystick_apply_event((struct js_event){
		.type = JS_EVENT_BUTTON,
		.number = 0,
		.value = 1,
	}, channels, &state);
	rc |= expect_true(state.arm_state, "button 0 press should toggle arm on");
	rc |= expect_true(channels[4] == ULAMA_CRSF_VALUE_MAX, "arm channel should go high on first press");

	ulama_joystick_apply_event((struct js_event){
		.type = JS_EVENT_BUTTON,
		.number = 0,
		.value = 0,
	}, channels, &state);
	ulama_joystick_apply_event((struct js_event){
		.type = JS_EVENT_BUTTON,
		.number = 0,
		.value = 1,
	}, channels, &state);
	rc |= expect_true(!state.arm_state, "second button 0 press should toggle arm off");
	rc |= expect_true(channels[4] == ULAMA_CRSF_VALUE_MIN, "arm channel should go low on second press");

	ulama_joystick_apply_event((struct js_event){
		.type = JS_EVENT_BUTTON,
		.number = 1,
		.value = 1,
	}, channels, &state);
	rc |= expect_true(channels[5] == ULAMA_CRSF_VALUE_MAX, "button 1 should drive aux channel high");

	ulama_joystick_apply_event((struct js_event){
		.type = JS_EVENT_BUTTON,
		.number = 1,
		.value = 0,
	}, channels, &state);
	rc |= expect_true(channels[5] == ULAMA_CRSF_VALUE_MIN, "button 1 release should drive aux channel low");

	if (rc != 0) {
		return 1;
	}
	printf("test_joystick_mapping: ok\n");
	return 0;
}