#include "fusion.h"
#include <string.h>

#define DEG2RAD            0.017453292f
#define BLOCK_TIME         0.5f   /* seconds per rest-detection block */
#define CALIB_BLOCKS       4      /* consecutive still blocks needed (2 s) */
#define CALIB_FALLBACK_T   12.0f  /* after this, accept the stillest block seen */
#define REST_GYRO_STD      (0.8f * DEG2RAD)
#define REST_ACC_STD       0.03f  /* g */
#define REST_MAX_DRIFT     (2.0f * DEG2RAD)
#define FALLBACK_GYRO_STD  (4.0f * DEG2RAD)
#define DEFAULT_ACC_SCALE  512.0f /* raw units per g on the 3DS accelerometer */

void fusion_init(Fusion* f) {
	memset(f, 0, sizeof(*f));
	f->q = q_identity();
	f->accScale = DEFAULT_ACC_SCALE;
	f->bestVar = 1e9f;
}

static void reset_block(Fusion* f) {
	f->gSum = f->gSumSq = f->aVecSum = v3(0, 0, 0);
	f->aSum = f->aSumSq = 0;
	f->blockCount = 0;
	f->blockTime = 0;
}

static void end_block(Fusion* f) {
	float n = (float)f->blockCount;
	vec3 mean = v3_scale(f->gSum, 1.0f / n);
	vec3 var = v3(f->gSumSq.x / n - mean.x * mean.x, f->gSumSq.y / n - mean.y * mean.y,
				  f->gSumSq.z / n - mean.z * mean.z);
	float gVar = fmaxf(var.x, fmaxf(var.y, var.z));
	float aMean = f->aSum / n;
	float aVar = (f->aSumSq / n - aMean * aMean) / (f->accScale * f->accScale);

	bool still = gVar < REST_GYRO_STD * REST_GYRO_STD && aVar < REST_ACC_STD * REST_ACC_STD;

	if (!f->calibrated) {
		if (gVar < f->bestVar) {
			f->bestVar = gVar;
			f->bestMean = mean;
			f->bestAcc = aMean;
		}
		if (still) {
			f->restBlocks++;
			f->calibGyroSum = v3_add(f->calibGyroSum, mean);
			f->calibAccSum += aMean;
			if (f->restBlocks >= CALIB_BLOCKS) {
				f->bias = v3_scale(f->calibGyroSum, 1.0f / CALIB_BLOCKS);
				f->accScale = f->calibAccSum / CALIB_BLOCKS;
				f->calibrated = true;
			}
		} else {
			f->restBlocks = 0;
			f->calibGyroSum = v3(0, 0, 0);
			f->calibAccSum = 0;
			if (f->elapsed > CALIB_FALLBACK_T && f->bestVar < FALLBACK_GYRO_STD * FALLBACK_GYRO_STD) {
				f->bias = f->bestMean;
				f->calibrated = true;
			}
		}
		f->calibProgress = f->calibrated ? 1.0f : (float)f->restBlocks / CALIB_BLOCKS;
		f->atRest = still;
		return;
	}

	/* Continuous bias tracking: only while really still and close to the known bias,
	   so that a slow deliberate rotation is not absorbed. */
	f->atRest = still && v3_len(v3_sub(mean, f->bias)) < REST_MAX_DRIFT;
	if (f->atRest) {
		f->bias = v3_add(f->bias, v3_scale(v3_sub(mean, f->bias), 0.25f));
		f->accScale += (aMean - f->accScale) * 0.1f;
	}
}

void fusion_update(Fusion* f, vec3 gyroRad, vec3 accRaw, float dt) {
	if (dt < 0.001f) dt = 0.001f;
	if (dt > 0.05f) dt = 0.05f;
	f->elapsed += dt;

	float aMag = v3_len(accRaw);

	/* rest-detection statistics on raw data */
	f->gSum = v3_add(f->gSum, gyroRad);
	f->gSumSq = v3_add(f->gSumSq, v3(gyroRad.x * gyroRad.x, gyroRad.y * gyroRad.y, gyroRad.z * gyroRad.z));
	f->aSum += aMag;
	f->aSumSq += aMag * aMag;
	f->blockCount++;
	f->blockTime += dt;
	if (f->blockTime >= BLOCK_TIME) {
		end_block(f);
		reset_block(f);
	}

	if (!f->initialized) {
		if (aMag < 1e-3f) return;
		/* Initial attitude straight from gravity, yaw arbitrary. */
		f->q = q_from_two_vectors(v3_scale(accRaw, 1.0f / aMag), v3(0, 0, 1));
		f->initialized = true;
		return;
	}

	vec3 w = v3_sub(gyroRad, f->bias);

	float aG = aMag / f->accScale;
	if (aG > 0.75f && aG < 1.25f) {
		float kp;
		if (f->elapsed < 3.0f) kp = 5.0f;       /* converge quickly at startup */
		else if (f->atRest) kp = 1.0f;
		else kp = 0.35f;
		/* weight down the correction when linear acceleration is significant */
		kp *= 1.0f - fminf(fabsf(aG - 1.0f) * 4.0f, 1.0f) * 0.9f;

		vec3 a = v3_scale(accRaw, 1.0f / aMag);
		/* expected "up" in the body frame */
		quat q = f->q;
		vec3 v = v3(2.0f * (q.x * q.z - q.w * q.y),
					2.0f * (q.w * q.x + q.y * q.z),
					q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z);
		vec3 e = v3_cross(a, v);
		w = v3_add(w, v3_scale(e, kp));
	}

	f->q = q_norm(q_mul(f->q, q_from_rotvec(v3_scale(w, dt))));
}
