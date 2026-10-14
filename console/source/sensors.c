#include "sensors.h"
#include <stddef.h>

#define DEG2RAD 0.017453292f

typedef struct {
	s64 tick;
	s64 prevTick;
	u32 index;
	u32 pad;
	s16 raw[3];
	s16 pad2;
	s16 entries[8][3];
} HidAccelSection;

typedef struct {
	s64 tick;
	s64 prevTick;
	u32 index;
	u32 pad;
	s16 raw[3];
	s16 pad2;
	s16 entries[32][3];
} HidGyroSection;

_Static_assert(sizeof(HidAccelSection) == 0x50, "accel section size");
_Static_assert(sizeof(HidGyroSection) == 0xE0, "gyro section size");

#define ACCEL_SECTION ((volatile HidAccelSection*)((u8*)hidSharedMem + 0x108))
#define GYRO_SECTION  ((volatile HidGyroSection*)((u8*)hidSharedMem + 0x158))

static float s_coef = 14.375f; /* raw units per deg/s */
static u32 s_lastIdx;
static s64 s_lastTick;
static float s_period = 0.0099f;

bool sensors_init(void) {
	if (R_FAILED(HIDUSER_EnableAccelerometer())) return false;
	if (R_FAILED(HIDUSER_EnableGyroscope())) return false;
	float c = 0;
	if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&c)) && c > 1.0f && c < 100.0f)
		s_coef = c;
	s_lastIdx = GYRO_SECTION->index & 31;
	s_lastTick = GYRO_SECTION->tick;
	return true;
}

void sensors_exit(void) {
	HIDUSER_DisableGyroscope();
	HIDUSER_DisableAccelerometer();
}

static void update_period(void) {
	volatile HidGyroSection* g = GYRO_SECTION;
	s64 tick = g->tick, prev = g->prevTick;
	if (tick == s_lastTick || prev == 0 || tick <= prev) return;
	s_lastTick = tick;
	/* tick is updated each time the ring wraps (32 samples) */
	float p = (float)(tick - prev) / 32.0f / (float)SYSCLOCK_ARM11;
	if (p > 0.002f && p < 0.03f) s_period += (p - s_period) * 0.2f;
}

int sensors_read_gyro(vec3* out, int max) {
	volatile HidGyroSection* g = GYRO_SECTION;
	u32 idx = g->index & 31;
	int n = (int)((idx - s_lastIdx) & 31);
	if (n > max) {
		/* fell behind: keep only the most recent samples */
		s_lastIdx = (idx - max) & 31;
		n = max;
	}
	for (int i = 0; i < n; i++) {
		s_lastIdx = (s_lastIdx + 1) & 31;
		volatile s16* e = g->entries[s_lastIdx];
		float k = DEG2RAD / s_coef;
		out[i] = v3(e[0] * k, e[1] * k, e[2] * k);
	}
	update_period();
	return n;
}

vec3 sensors_read_accel(void) {
	volatile HidAccelSection* a = ACCEL_SECTION;
	u32 idx = a->index & 7;
	volatile s16* e = a->entries[idx];
	/* the 3DS reports the gravity direction; negate to get the specific force */
	return v3(-(float)e[0], -(float)e[1], -(float)e[2]);
}

float sensors_gyro_period(void) {
	return s_period;
}
