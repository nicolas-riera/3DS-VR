#include "wifilock.h"

#include <3ds.h>

static bool s_ndm;
static bool s_locked;
static Handle s_connectEvent;
static aptHookCookie s_hook;

static void acquire(void) {
	if (!s_ndm || s_locked) return;
	NDMU_SuspendDaemons(NDM_DAEMON_MASK_ALL);
	NDMU_SuspendScheduler(1);
	if (R_SUCCEEDED(NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE)))
		s_locked = R_SUCCEEDED(NDMU_LockState());
}

static void release(void) {
	if (!s_ndm) return;
	if (s_locked) NDMU_UnlockState();
	NDMU_LeaveExclusiveState();
	NDMU_ResumeScheduler();
	NDMU_ResumeDaemons(NDM_DAEMON_MASK_ALL);
	s_locked = false;
}

/* The HOME Menu and applets need NDM: give it back while we are suspended. */
static void on_apt(APT_HookType hook, void* param) {
	(void)param;
	if (hook == APTHOOK_ONSUSPEND) release();
	else if (hook == APTHOOK_ONRESTORE) acquire();
}

void wifilock_begin(void) {
	if (R_FAILED(ndmuInit())) return;
	s_ndm = true;
	acquire();
	aptHook(&s_hook, on_apt, NULL);
}

void wifilock_end(void) {
	if (!s_ndm) return;
	aptUnhook(&s_hook);
	release();
	ndmuExit();
	if (s_connectEvent) svcCloseHandle(s_connectEvent);
	s_connectEvent = 0;
	s_ndm = false;
}

bool wifilock_active(void) {
	return s_locked;
}

void wifilock_reconnect(void) {
	if (!s_connectEvent && R_FAILED(svcCreateEvent(&s_connectEvent, RESET_ONESHOT))) return;
	acuConfig cfg;
	if (R_FAILED(ACU_CreateDefaultConfig(&cfg))) return;
	ACU_ConnectAsync(&cfg, s_connectEvent);
}
