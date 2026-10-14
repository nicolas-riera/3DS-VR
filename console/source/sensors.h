#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "mathx.h"

/*
 * Direct access to the accelerometer / gyroscope ring buffers in HID shared
 * memory, so that every gyro sample (~100 Hz) is used by the filter.
 *
 * Sensor frame (right-handed, same convention as the Citra/Azahar emulators,
 * both sensors share it):
 *   +X : towards the left of the console
 *   +Y : out of the touch screen (up when the console lies open on a table)
 *   +Z : from the player towards the hinge
 * Gyroscope: angular rate around those axes (right-hand rule), raw / coef = deg/s.
 * Accelerometer: gravity direction (reads -Y when lying flat), ~512 raw per g.
 */

bool sensors_init(void);
void sensors_exit(void);

/* Pops the gyro samples produced since the last call (rad/s, sensor frame). */
int sensors_read_gyro(vec3* out, int max);
/* Latest accelerometer sample as specific force (points up at rest), raw units. */
vec3 sensors_read_accel(void);
/* Gyro sample period in seconds, measured from HID timestamps. */
float sensors_gyro_period(void);
