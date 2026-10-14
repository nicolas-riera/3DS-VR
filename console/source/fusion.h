#pragma once
#include <stdbool.h>
#include "mathx.h"

/*
 * 6-axis orientation filter (Mahony complementary filter) with automatic
 * gyroscope bias calibration based on rest detection.
 *
 * Body frame: the 3DS sensor frame (right-handed, see sensors.h).
 * World frame: Z up, as expected by the SlimeVR protocol.
 * q rotates body vectors into the world frame.
 */
typedef struct {
	quat q;
	bool initialized;

	vec3 bias;          /* gyro bias, rad/s */
	bool calibrated;    /* initial bias calibration done */
	float calibProgress;/* 0..1 */
	bool atRest;
	float accScale;     /* raw accel magnitude measured at rest (raw units per g) */

	/* rest detection block statistics */
	vec3 gSum, gSumSq;
	float aSum, aSumSq;
	vec3 aVecSum;
	int blockCount;
	float blockTime;
	int restBlocks;
	vec3 calibGyroSum;
	float calibAccSum;
	float elapsed;
	float bestVar;
	vec3 bestMean;
	float bestAcc;
} Fusion;

void fusion_init(Fusion* f);

/*
 * gyroRad: angular rate in rad/s (raw, bias not removed), body frame.
 * accRaw:  specific force (pointing up at rest) in raw units, body frame.
 * dt:      seconds since previous sample.
 */
void fusion_update(Fusion* f, vec3 gyroRad, vec3 accRaw, float dt);
