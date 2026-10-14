#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "mathx.h"
#include "vr3ds_protocol.h"

/*
 * Head tracking thread: gyroscope + accelerometer fusion (3DoF), recentering,
 * battery polling, and the 100 Hz TRACKING packets (which also carry the
 * gamepad state).
 *
 * Orientations use SteamVR's head axes: +X right, +Y up, +Z towards the
 * player (so the view direction is -Z). The model assumes the bottom half of
 * the console is held in front of the face and the hinge is horizontal: the
 * hinge angle is unknown, so recentering solves for it (see tracking.c).
 */

bool tracking_start(void);
void tracking_stop(void);

/* Latest recentered head orientation. */
quat tracking_orientation(void);
/* Makes the current pose "looking straight ahead, level". */
void tracking_recenter(void);

bool tracking_sensors_ok(void);
/* Gyroscope bias calibration (needs the console to be still for ~2 s). */
bool tracking_calibrated(float* progress);
void tracking_battery(u8* percent, u8* flags);

/* Inputs to send with the next TRACKING packets. */
void tracking_set_pad(const Vr3dsGamepad* pad, u8 stereoStrength);
