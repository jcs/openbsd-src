/*
 * Copyright (c) 2016 joshua stein <jcs@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _MACHINE_CHROMEECVAR_H_
#define _MACHINE_CHROMEECVAR_H_

struct chromeec_attach_args {
	const char *checaa_name;
};

struct chromeec_led_rgb {
	uint8_t led, red, green, blue;
};

struct chromeec_rgb_s {
	uint8_t r, g, b;
};

struct chromeec_lightbar_params_v1 {
	/* Timing */
	int32_t google_ramp_up;
	int32_t google_ramp_down;
	int32_t s3s0_ramp_up;
	int32_t s0_tick_delay[2];		/* AC=0/1 */
	int32_t s0a_tick_delay[2];		/* AC=0/1 */
	int32_t s0s3_ramp_down;
	int32_t s3_sleep_for;
	int32_t s3_ramp_up;
	int32_t s3_ramp_down;
	int32_t s5_ramp_up;
	int32_t s5_ramp_down;
	int32_t tap_tick_delay;
	int32_t tap_gate_delay;
	int32_t tap_display_time;

	/* Tap-for-battery params */
	uint8_t tap_pct_red;
	uint8_t tap_pct_green;
	uint8_t tap_seg_min_on;
	uint8_t tap_seg_max_on;
	uint8_t tap_seg_osc;
	uint8_t tap_idx[3];

	/* Oscillation */
	uint8_t osc_min[2];			/* AC=0/1 */
	uint8_t osc_max[2];			/* AC=0/1 */
	uint8_t w_ofs[2];			/* AC=0/1 */

	/* Brightness limits based on the backlight and AC. */
	uint8_t bright_bl_off_fixed[2];		/* AC=0/1 */
	uint8_t bright_bl_on_min[2];		/* AC=0/1 */
	uint8_t bright_bl_on_max[2];		/* AC=0/1 */

	/* Battery level thresholds */
# define LB_BATTERY_LEVELS 4
	uint8_t battery_threshold[LB_BATTERY_LEVELS - 1];

	/* Map [AC][battery_level] to color index */
	uint8_t s0_idx[2][LB_BATTERY_LEVELS];	/* AP is running */
	uint8_t s3_idx[2][LB_BATTERY_LEVELS];	/* AP is sleeping */

	/* s5: single color pulse on inhibited power-up */
	uint8_t s5_idx;

	/* Color palette */
	struct chromeec_rgb_s color[8];		/* 0-3 s0 colors, 4-7 b/r/y/g */
} __packed;

struct chromeec_lightbar_program {
	uint8_t size;
# define CHROMEEC_LIGHTBAR_PROGRAM_LEN 192
	uint8_t data[CHROMEEC_LIGHTBAR_PROGRAM_LEN];
# define CHROMEEC_LIGHTBAR_OPCODE_ON			0
# define CHROMEEC_LIGHTBAR_OPCODE_OFF			1
# define CHROMEEC_LIGHTBAR_OPCODE_JUMP			2
# define CHROMEEC_LIGHTBAR_OPCODE_JUMP_BATTERY		3
# define CHROMEEC_LIGHTBAR_OPCODE_JUMP_IF_CHARGING	4
# define CHROMEEC_LIGHTBAR_OPCODE_SET_WAIT_DELAY	5
# define CHROMEEC_LIGHTBAR_OPCODE_SET_RAMP_DELAY	6
# define CHROMEEC_LIGHTBAR_OPCODE_WAIT			7
# define CHROMEEC_LIGHTBAR_OPCODE_SET_BRIGHTNESS	8
# define CHROMEEC_LIGHTBAR_OPCODE_SET_COLOR_SINGLE	9
# define CHROMEEC_LIGHTBAR_OPCODE_SET_COLOR_RGB		10
# define CHROMEEC_LIGHTBAR_OPCODE_GET_COLORS		11
# define CHROMEEC_LIGHTBAR_OPCODE_SWAP_COLORS		12
# define CHROMEEC_LIGHTBAR_OPCODE_RAMP_ONCE		13
# define CHROMEEC_LIGHTBAR_OPCODE_CYCLE_ONCE		14
# define CHROMEEC_LIGHTBAR_OPCODE_CYCLE			15
# define CHROMEEC_LIGHTBAR_OPCODE_HALT			16
};

#define CHROMEEC_IOC_LIGHTBAR_SET_POWER		_IOW('C', 1, uint8_t)

#define CHROMEEC_IOC_LIGHTBAR_INIT		_IO('C', 2)

#define CHROMEEC_IOC_LIGHTBAR_GET_BRIGHTNESS	_IOWR('C', 3, uint8_t)
#define CHROMEEC_IOC_LIGHTBAR_SET_BRIGHTNESS	_IOW('C', 4, uint8_t)

#define CHROMEEC_IOC_LIGHTBAR_GET_RGB		_IOWR('C', 5, struct chromeec_led_rgb)
#define CHROMEEC_IOC_LIGHTBAR_SET_RGB		_IOW('C', 6, struct chromeec_led_rgb)

#define CHROMEEC_IOC_LIGHTBAR_GET_SEQ		_IOWR('C', 7, uint8_t)
#define CHROMEEC_IOC_LIGHTBAR_SET_SEQ		_IOW('C', 8, uint8_t)
# define CHROMEEC_LIGHTBAR_SEQ_ERROR		0
# define CHROMEEC_LIGHTBAR_SEQ_S5		1
# define CHROMEEC_LIGHTBAR_SEQ_S3		2
# define CHROMEEC_LIGHTBAR_SEQ_S0		3
# define CHROMEEC_LIGHTBAR_SEQ_S5S3		4
# define CHROMEEC_LIGHTBAR_SEQ_S3S0		5
# define CHROMEEC_LIGHTBAR_SEQ_S0S3		6
# define CHROMEEC_LIGHTBAR_SEQ_S3S5		7
# define CHROMEEC_LIGHTBAR_SEQ_STOP		8
# define CHROMEEC_LIGHTBAR_SEQ_RUN		9
# define CHROMEEC_LIGHTBAR_SEQ_KONAMI		10
# define CHROMEEC_LIGHTBAR_SEQ_TAP		11
# define CHROMEEC_LIGHTBAR_SEQ_PROGRAM		12

#define CHROMEEC_IOC_LIGHTBAR_GET_DEMO		_IOWR('C', 9, uint8_t)
#define CHROMEEC_IOC_LIGHTBAR_SET_DEMO		_IOW('C', 10, uint8_t)

#define CHROMEEC_IOC_LIGHTBAR_GET_PARAMS_V1	_IOWR('C', 11, struct chromeec_lightbar_params_v1)
#define CHROMEEC_IOC_LIGHTBAR_SET_PARAMS_V1	_IOW('C', 12, struct chromeec_lightbar_params_v1)

#define CHROMEEC_IOC_LIGHTBAR_SET_PROGRAM	_IOW('C', 13, struct chromeec_lightbar_program)

int	chromeec_probe(void);

#endif
