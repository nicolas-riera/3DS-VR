#include "tracking.h"

#include <string.h>

#include "fusion.h"
#include "net.h"
#include "sensors.h"
#include "sysinfo.h"
#include "video.h"

#define SQRT_HALF 0.70710678f

static Thread s_thread;
static volatile bool s_running;
static LightLock s_lock;
static Fusion s_fusion;
static bool s_sensorsOk;
static volatile bool s_suspended, s_paused;
static aptHookCookie s_hook;

static quat s_head = {1, 0, 0, 0};  /* recentered output */
static quat s_yawFix = {1, 0, 0, 0};
static quat s_mount = {1, 0, 0, 0}; /* hinge correction, applied in the head frame */
static volatile bool s_recenterReq;
static bool s_wasCalibrated;

static Vr3dsGamepad s_pad;
static u8 s_stereoStrength;
static u8 s_battery = 100, s_batteryFlags;

/*
 * Sensor frame (see sensors.h): +X left, +Y out of the touch screen, +Z
 * towards the hinge. With the bottom half held upright in front of the face,
 * that is head -X, +Z and +Y: a 180 degree turn around (0, 1, 1).
 * The fusion world is Z-up; SteamVR's is Y-up: -90 degrees around X.
 */
static const quat BODY_TO_HEAD = {0, 0, SQRT_HALF, SQRT_HALF};
static const quat WORLD_ZUP_TO_YUP = {SQRT_HALF, -SQRT_HALF, 0, 0};

static quat head_from_fusion(quat q) {
	return q_norm(q_mul(WORLD_ZUP_TO_YUP, q_mul(q, q_conj(BODY_TO_HEAD))));
}

/*
 * The gyroscope is in the bottom half but the user looks at the top screen,
 * at an unknown hinge angle. Recentering assumes the user is looking straight
 * ahead: it finds the rotation around the hinge (head X axis) that makes the
 * view direction horizontal, keeps the roll given by gravity, then removes
 * the heading.
 */
static void recenter(quat q) {
	vec3 y = q_rotate(q, v3(0, 1, 0));
	vec3 z = q_rotate(q, v3(0, 0, 1));
	float a = y.y, b = z.y;
	/* forward(t) = q * Rx(t) * (0,0,-1) is horizontal when sin(t)*a = cos(t)*b,
	   and this solution keeps "up" pointing up */
	if (a * a + b * b > 0.05f) s_mount = q_from_axis_angle(v3(1, 0, 0), atan2f(b, a));

	quat m = q_mul(q, s_mount);
	vec3 f = q_rotate(m, v3(0, 0, -1));
	if (f.x * f.x + f.z * f.z > 1e-4f) {
		float heading = atan2f(-f.x, -f.z);
		s_yawFix = q_from_axis_angle(v3(0, 1, 0), -heading);
	}
}

static void worker(void* arg) {
	(void)arg;
	vec3 samples[32];
	u64 lastSend = 0, lastBattery = 0;
	u32 seq = 0;

	while (s_running) {
		if (s_suspended) {
			s_paused = true;
			svcSleepThread(50 * 1000000LL);
			continue;
		}
		s_paused = false;

		u64 now = osGetTime();
		int n = s_sensorsOk ? sensors_read_gyro(samples, 32) : 0;
		vec3 acc = s_sensorsOk ? sensors_read_accel() : v3(0, 0, 0);
		float dt = sensors_gyro_period();
		for (int i = 0; i < n; i++) fusion_update(&s_fusion, samples[i], acc, dt);

		if (now - lastBattery >= 5000 || lastBattery == 0) {
			lastBattery = now;
			u8 p, f;
			sysinfo_battery(&p, &f);
			LightLock_Lock(&s_lock);
			s_battery = p;
			s_batteryFlags = f;
			LightLock_Unlock(&s_lock);
		}

		if (s_fusion.initialized) {
			quat raw = head_from_fusion(s_fusion.q);
			/* recenter once the gyro bias is known, and whenever asked */
			if (s_recenterReq || (s_fusion.calibrated && !s_wasCalibrated)) {
				s_recenterReq = false;
				recenter(raw);
			}
			s_wasCalibrated = s_fusion.calibrated;

			LightLock_Lock(&s_lock);
			s_head = q_norm(q_mul(s_yawFix, q_mul(raw, s_mount)));
			LightLock_Unlock(&s_lock);
		}

		/* ~100 Hz: one packet per gyro sample batch, at least every 20 ms */
		if (net_is_streaming() && (n > 0 || now - lastSend >= 20)) {
			lastSend = now;
			Vr3dsTracking t;
			memset(&t, 0, sizeof(t));
			LightLock_Lock(&s_lock);
			t.qw = s_head.w;
			t.qx = s_head.x;
			t.qy = s_head.y;
			t.qz = s_head.z;
			t.pad = s_pad;
			t.stereoStrength = s_stereoStrength;
			t.battery = s_battery;
			t.batteryFlags = s_batteryFlags;
			LightLock_Unlock(&s_lock);
			t.seq = seq++;
			t.wifi = osGetWifiStrength();
			VideoStats vs;
			video_stats(&vs);
			t.shownFrameId = vs.shownFrameId;
			t.shownFrameTs = vs.shownFrameTs;
			t.shownFrameAgeUs = vs.shownFrameAgeUs;
			t.framesDecoded = vs.framesDecoded;
			t.framesDropped = vs.framesDropped;
			net_send_tracking(&t);
		}

		svcSleepThread(2 * 1000000LL);
	}
}

static void on_apt(APT_HookType hook, void* param) {
	(void)param;
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
		s_suspended = true;
		for (int i = 0; i < 200 && s_thread && !s_paused; i++) svcSleepThread(1000000LL);
	} else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
		s_suspended = false;
	}
}

bool tracking_start(void) {
	LightLock_Init(&s_lock);
	fusion_init(&s_fusion);
	s_sensorsOk = sensors_init();
	s_running = true;
	aptHook(&s_hook, on_apt, NULL);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	/* above the main thread: tracking must never wait for video decoding */
	s_thread = threadCreate(worker, NULL, 16 * 1024, prio - 2, -2, false);
	return s_thread != NULL;
}

void tracking_stop(void) {
	if (!s_thread) return;
	s_running = false;
	threadJoin(s_thread, U64_MAX);
	threadFree(s_thread);
	s_thread = NULL;
	aptUnhook(&s_hook);
	if (s_sensorsOk) sensors_exit();
}

quat tracking_orientation(void) {
	LightLock_Lock(&s_lock);
	quat q = s_head;
	LightLock_Unlock(&s_lock);
	return q;
}

void tracking_recenter(void) {
	s_recenterReq = true;
}

bool tracking_sensors_ok(void) {
	return s_sensorsOk;
}

bool tracking_calibrated(float* progress) {
	if (progress) *progress = s_fusion.calibProgress;
	return s_fusion.calibrated;
}

void tracking_battery(u8* percent, u8* flags) {
	LightLock_Lock(&s_lock);
	*percent = s_battery;
	*flags = s_batteryFlags;
	LightLock_Unlock(&s_lock);
}

void tracking_set_pad(const Vr3dsGamepad* pad, u8 stereoStrength) {
	LightLock_Lock(&s_lock);
	s_pad = *pad;
	s_stereoStrength = stereoStrength;
	LightLock_Unlock(&s_lock);
}
