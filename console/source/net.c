#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio.h"
#include "settings.h"
#include "sysinfo.h"

#define SLOT_COUNT         6
#define MAX_FRAGS          (VR3DS_MAX_FRAME / VR3DS_VIDEO_PAYLOAD + 1)
#define SESSION_TIMEOUT_MS 3000
#define PAIR_TIMEOUT_MS    3000
#define REJECT_SHOW_MS     4000
#define HELLO_PERIOD_MS    500
#define SETTINGS_PERIOD_MS 1000
#define KEYREQ_MIN_MS      150

typedef enum { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_BUSY } SlotState;

typedef struct {
	SlotState state;
	u32 frameId;
	u16 configId;
	u8 flags;
	u16 fragCount, fragsGot;
	u32 size;
	u64 ts;
	quat pose;
	u64 recvTick;
	u8 got[(MAX_FRAGS + 7) / 8];
	u8* data;
} Slot;

static int s_sock = -1;
static Thread s_thread;
static volatile bool s_running;
static LightLock s_lock; /* status, PC address and slots */
static NetStatus s_st;
static struct sockaddr_in s_pc;
static bool s_hasPc;
static Slot s_slots[SLOT_COUNT];
static u32 s_lost;
static volatile bool s_settingsDirty, s_disconnectReq, s_reopen;
static u64 s_lastRx, s_lastHello, s_lastSettings, s_lastKeyReq, s_stateSince, s_lastIpCheck;
static u32 s_rxBytes;
static u64 s_rxWindow;
static u32 s_settingsSeq;
static u32 s_bcast;
static u32 s_leftSession;  /* session we just left: its CONFIG resends must not pull us back */
static u64 s_leftAt;
static aptHookCookie s_hook;
static u8 s_rx[2048];

/* wrap-safe "a is before b" for frame ids */
static inline bool id_before(u32 a, u32 b) {
	return (s32)(a - b) < 0;
}

/* ------------------------------------------------------------------ slots */

static void slots_reset_locked(void) {
	for (int i = 0; i < SLOT_COUNT; i++)
		if (s_slots[i].state != SLOT_BUSY) s_slots[i].state = SLOT_FREE;
}

static Slot* slot_for_frame_locked(const Vr3dsVideoHeader* v) {
	Slot* freeSlot = NULL;
	Slot* oldestFilling = NULL;
	Slot* oldestReady = NULL;
	for (int i = 0; i < SLOT_COUNT; i++) {
		Slot* s = &s_slots[i];
		if (s->state != SLOT_FREE && s->frameId == v->frameId && s->configId == v->configId)
			return s->state == SLOT_FILLING ? s : NULL; /* late duplicate of a done frame */
		if (s->state == SLOT_FREE && !freeSlot) freeSlot = s;
		if (s->state == SLOT_FILLING && (!oldestFilling || id_before(s->frameId, oldestFilling->frameId)))
			oldestFilling = s;
		if (s->state == SLOT_READY && (!oldestReady || id_before(s->frameId, oldestReady->frameId)))
			oldestReady = s;
	}
	Slot* s = freeSlot;
	if (!s && oldestFilling) {
		if (!id_before(v->frameId, oldestFilling->frameId)) {
			s = oldestFilling;
			s_lost++;
		}
	}
	if (!s && oldestReady) {
		/* the decoder is not keeping up */
		s = oldestReady;
		s_lost++;
	}
	if (!s) return NULL;
	s->state = SLOT_FILLING;
	s->frameId = v->frameId;
	s->configId = v->configId;
	s->flags = v->flags;
	s->fragCount = v->fragCount;
	s->fragsGot = 0;
	s->size = v->frameSize;
	s->ts = v->timestampUs;
	s->pose = (quat){v->qw, v->qx, v->qy, v->qz};
	memset(s->got, 0, sizeof(s->got));
	return s;
}

static void on_video(const u8* data, int len) {
	const Vr3dsVideoHeader* v = (const Vr3dsVideoHeader*)data;
	const u8* payload = data + sizeof(*v);
	int plen = len - (int)sizeof(*v);
	if (plen <= 0 || v->fragCount == 0 || v->fragCount > MAX_FRAGS || v->fragIndex >= v->fragCount ||
		v->frameSize == 0 || v->frameSize > VR3DS_MAX_FRAME)
		return;
	u32 offset = (u32)v->fragIndex * VR3DS_VIDEO_PAYLOAD;
	if (offset + (u32)plen > v->frameSize) return;

	LightLock_Lock(&s_lock);
	if (s_st.state == NET_STREAMING && s_st.hasConfig && v->configId == s_st.config.configId) {
		Slot* s = slot_for_frame_locked(v);
		if (s && !(s->got[v->fragIndex >> 3] & (1 << (v->fragIndex & 7)))) {
			s->got[v->fragIndex >> 3] |= 1 << (v->fragIndex & 7);
			memcpy(s->data + offset, payload, plen);
			if (++s->fragsGot == s->fragCount) {
				s->state = SLOT_READY;
				s->recvTick = svcGetSystemTick();
				/* anything older still incomplete will never be decodable */
				for (int i = 0; i < SLOT_COUNT; i++) {
					Slot* o = &s_slots[i];
					if (o->state == SLOT_FILLING && id_before(o->frameId, s->frameId)) {
						o->state = SLOT_FREE;
						s_lost++;
					}
				}
			}
		}
	}
	LightLock_Unlock(&s_lock);
}

bool net_pop_frame(NetFrame* out, bool latestOnly) {
	bool found = false;
	LightLock_Lock(&s_lock);
	Slot* pick = NULL;
	for (int i = 0; i < SLOT_COUNT; i++) {
		Slot* s = &s_slots[i];
		if (s->state != SLOT_READY) continue;
		if (!pick || (latestOnly ? id_before(pick->frameId, s->frameId) : id_before(s->frameId, pick->frameId)))
			pick = s;
	}
	if (pick) {
		if (latestOnly) {
			for (int i = 0; i < SLOT_COUNT; i++) {
				Slot* s = &s_slots[i];
				if (s != pick && s->state == SLOT_READY && id_before(s->frameId, pick->frameId)) {
					s->state = SLOT_FREE;
					s_lost++;
				}
			}
		}
		pick->state = SLOT_BUSY;
		out->frameId = pick->frameId;
		out->configId = pick->configId;
		out->flags = pick->flags;
		out->size = pick->size;
		out->timestampUs = pick->ts;
		out->pose = pick->pose;
		out->recvTick = pick->recvTick;
		out->data = pick->data;
		out->slot = (int)(pick - s_slots);
		found = true;
	}
	LightLock_Unlock(&s_lock);
	return found;
}

void net_release_frame(const NetFrame* f) {
	LightLock_Lock(&s_lock);
	if (f->slot >= 0 && f->slot < SLOT_COUNT) s_slots[f->slot].state = SLOT_FREE;
	LightLock_Unlock(&s_lock);
}

u32 net_frames_lost(void) {
	return s_lost;
}

/* --------------------------------------------------------------- sending */

static void send_to(const void* data, int len, const struct sockaddr_in* to) {
	if (s_sock < 0) return;
	sendto(s_sock, data, len, 0, (const struct sockaddr*)to, sizeof(*to));
}

static bool pc_addr(struct sockaddr_in* out) {
	LightLock_Lock(&s_lock);
	bool ok = s_hasPc;
	*out = s_pc;
	LightLock_Unlock(&s_lock);
	return ok;
}

static void send_to_pc(const void* data, int len) {
	struct sockaddr_in pc;
	if (pc_addr(&pc)) send_to(data, len, &pc);
}

static void send_hello(void) {
	Vr3dsHello h;
	memset(&h, 0, sizeof(h));
	vr3ds_header(&h.h, VR3DS_PKT_HELLO);
	h.consoleId = g_console.consoleId;
	h.token = g_console.token;
	h.model = g_console.model;
	h.caps = (g_console.hwH264 ? VR3DS_CAP_HW_H264 : 0) | (g_console.has3D ? VR3DS_CAP_STEREO : 0);
	h.appVersion[0] = APP_VERSION_MAJOR;
	h.appVersion[1] = APP_VERSION_MINOR;
	h.appVersion[2] = APP_VERSION_MICRO;
	memcpy(h.name, g_console.name, sizeof(h.name));
	settings_stream(&h.settings);

	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(VR3DS_PORT_STREAMER);
	to.sin_addr.s_addr = 0xFFFFFFFF;
	send_to(&h, sizeof(h), &to);
	if (s_bcast && s_bcast != 0xFFFFFFFF) {
		to.sin_addr.s_addr = s_bcast;
		send_to(&h, sizeof(h), &to);
	}
	/* also straight to the last PC, for networks that filter broadcasts */
	send_to_pc(&h, sizeof(h));
}

static void send_settings(void) {
	Vr3dsSettingsPacket p;
	memset(&p, 0, sizeof(p));
	vr3ds_header(&p.h, VR3DS_PKT_SETTINGS);
	p.seq = s_settingsSeq++;
	settings_stream(&p.settings);
	send_to_pc(&p, sizeof(p));
}

static void send_bye(void) {
	Vr3dsHeader h;
	vr3ds_header(&h, VR3DS_PKT_BYE);
	for (int i = 0; i < 3; i++) send_to_pc(&h, sizeof(h)); /* datagrams get lost */
}

void net_send_tracking(Vr3dsTracking* t) {
	vr3ds_header(&t->h, VR3DS_PKT_TRACKING);
	send_to_pc(t, sizeof(*t));
}

void net_request_keyframe(u16 configId) {
	u64 now = osGetTime();
	if (now - s_lastKeyReq < KEYREQ_MIN_MS) return;
	s_lastKeyReq = now;
	Vr3dsKeyframeRequest r;
	vr3ds_header(&r.h, VR3DS_PKT_KEYFRAME_REQ);
	r.configId = configId;
	send_to_pc(&r, sizeof(r));
}

void net_settings_changed(void) {
	s_settingsDirty = true;
}

void net_disconnect(void) {
	s_disconnectReq = true;
}

/* ----------------------------------------------------------------- state */

static void set_state_locked(NetState st) {
	if (s_st.state == st) return;
	if (s_st.state == NET_STREAMING) {
		s_st.hasConfig = false;
		slots_reset_locked();
		audio_reset();
	}
	s_st.state = st;
	s_stateSince = osGetTime();
}

static void copy_name(char* dst, const char* src) {
	memcpy(dst, src, VR3DS_NAME_LEN);
	dst[VR3DS_NAME_LEN] = 0;
}

static void on_packet(const u8* data, int len, const struct sockaddr_in* from) {
	if (!vr3ds_check(data, len, sizeof(Vr3dsHeader))) return;
	const Vr3dsHeader* h = (const Vr3dsHeader*)data;
	u64 now = osGetTime();

	LightLock_Lock(&s_lock);
	bool fromPc = s_hasPc && from->sin_addr.s_addr == s_pc.sin_addr.s_addr;
	if (s_st.state == NET_STREAMING && fromPc) s_lastRx = now;
	NetState st = s_st.state;
	LightLock_Unlock(&s_lock);

	switch (h->type) {
	case VR3DS_PKT_PAIR_STATUS: {
		if (len < (int)sizeof(Vr3dsPairStatus) || st == NET_STREAMING) break;
		const Vr3dsPairStatus* p = (const Vr3dsPairStatus*)data;
		LightLock_Lock(&s_lock);
		s_pc = *from;
		s_pc.sin_port = htons(VR3DS_PORT_STREAMER);
		s_hasPc = true;
		copy_name(s_st.pcName, p->pcName);
		s_st.pairCode = p->code;
		s_lastRx = now;
		set_state_locked(p->state == VR3DS_PAIR_REJECTED ? NET_REJECTED : NET_PAIRING);
		LightLock_Unlock(&s_lock);
		break;
	}
	case VR3DS_PKT_CONFIG: {
		if (len < (int)sizeof(Vr3dsConfig)) break;
		const Vr3dsConfig* c = (const Vr3dsConfig*)data;
		if (c->eyeWidth == 0 || c->eyeHeight == 0 || c->eyeWidth > 512 || c->eyeHeight * (c->stereo ? 2 : 1) > 512)
			break;
		LightLock_Lock(&s_lock);
		/* while streaming, only the current PC may change the session */
		bool leftRecently = c->sessionId == s_leftSession && now - s_leftAt < 5000;
		if ((st != NET_STREAMING || fromPc) && !leftRecently) {
			bool newSession = st != NET_STREAMING || c->sessionId != s_st.sessionId;
			if (newSession) {
				set_state_locked(NET_SEARCHING);
				s_pc = *from;
				s_pc.sin_port = htons(VR3DS_PORT_STREAMER);
				s_hasPc = true;
				s_st.sessionId = c->sessionId;
				set_state_locked(NET_STREAMING);
				s_settingsDirty = true;
			}
			if (!s_st.hasConfig || s_st.config.configId != c->configId) slots_reset_locked();
			s_st.config = *c;
			s_st.hasConfig = true;
			copy_name(s_st.pcName, c->pcName);
			s_lastRx = now;
		}
		LightLock_Unlock(&s_lock);
		break;
	}
	case VR3DS_PKT_VIDEO:
		if (fromPc && len > (int)sizeof(Vr3dsVideoHeader)) on_video(data, len);
		break;
	case VR3DS_PKT_AUDIO:
		if (fromPc && st == NET_STREAMING && len >= (int)sizeof(Vr3dsAudioHeader)) {
			const Vr3dsAudioHeader* a = (const Vr3dsAudioHeader*)data;
			int dlen = len - (int)sizeof(*a);
			if (a->frames <= dlen) audio_submit(a, data + sizeof(*a));
		}
		break;
	case VR3DS_PKT_BYE:
		if (fromPc) {
			LightLock_Lock(&s_lock);
			set_state_locked(NET_SEARCHING);
			LightLock_Unlock(&s_lock);
		}
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- socket */

static void close_socket(void) {
	if (s_sock >= 0) close(s_sock);
	s_sock = -1;
}

static bool open_socket(void) {
	close_socket();
	int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s < 0) return false;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	int rcvbuf = 0x40000; /* room for a whole key frame; not every firmware accepts it */
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(VR3DS_PORT_CONSOLE);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(s);
		return false;
	}
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	s_sock = s;
	return true;
}

static void refresh_ip(void) {
	struct in_addr ip, mask, bcast;
	u32 local = 0;
	if (SOCU_GetIPInfo(&ip, &mask, &bcast) == 0) {
		local = ip.s_addr;
		s_bcast = bcast.s_addr ? bcast.s_addr : (ip.s_addr | ~mask.s_addr);
	} else {
		local = (u32)gethostid();
	}
	LightLock_Lock(&s_lock);
	s_st.localIp = local;
	if (local == 0) {
		set_state_locked(NET_OFFLINE);
	} else if (s_st.state == NET_OFFLINE) {
		set_state_locked(NET_SEARCHING);
	}
	LightLock_Unlock(&s_lock);
}

static void tick(u64 now) {
	if (now - s_lastIpCheck >= 2000 || s_lastIpCheck == 0) {
		s_lastIpCheck = now;
		refresh_ip();
		if (s_sock < 0 && s_st.state != NET_OFFLINE) open_socket();
	}

	if (s_disconnectReq) {
		s_disconnectReq = false;
		if (s_st.state == NET_STREAMING) {
			send_bye();
			s_leftSession = s_st.sessionId;
			s_leftAt = now;
		}
		LightLock_Lock(&s_lock);
		set_state_locked(NET_SEARCHING);
		LightLock_Unlock(&s_lock);
	}

	LightLock_Lock(&s_lock);
	NetState st = s_st.state;
	if (st == NET_STREAMING && now - s_lastRx > SESSION_TIMEOUT_MS) st = NET_SEARCHING;
	else if (st == NET_PAIRING && now - s_lastRx > PAIR_TIMEOUT_MS) st = NET_SEARCHING;
	else if (st == NET_REJECTED && now - s_stateSince > REJECT_SHOW_MS) st = NET_SEARCHING;
	set_state_locked(st);
	LightLock_Unlock(&s_lock);

	if (st == NET_SEARCHING || st == NET_PAIRING) {
		if (now - s_lastHello >= HELLO_PERIOD_MS) {
			s_lastHello = now;
			send_hello();
		}
	} else if (st == NET_STREAMING) {
		if (s_settingsDirty || now - s_lastSettings >= SETTINGS_PERIOD_MS) {
			s_settingsDirty = false;
			s_lastSettings = now;
			send_settings();
		}
	}

	if (now - s_rxWindow >= 1000) {
		LightLock_Lock(&s_lock);
		s_st.rxKbps = (float)s_rxBytes * 8.0f / (float)(now - s_rxWindow);
		LightLock_Unlock(&s_lock);
		s_rxBytes = 0;
		s_rxWindow = now;
	}
}

static void worker(void* arg) {
	(void)arg;
	while (s_running) {
		if (s_reopen) {
			s_reopen = false;
			close_socket();
			s_lastIpCheck = 0;
		}
		if (s_sock >= 0) {
			struct pollfd pfd = {s_sock, POLLIN, 0};
			if (poll(&pfd, 1, 5) > 0 && (pfd.revents & POLLIN)) {
				for (int i = 0; i < 64; i++) {
					struct sockaddr_in from;
					socklen_t fl = sizeof(from);
					int n = recvfrom(s_sock, s_rx, sizeof(s_rx), 0, (struct sockaddr*)&from, &fl);
					if (n <= 0) break;
					s_rxBytes += (u32)n + 28;
					on_packet(s_rx, n, &from);
				}
			} else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				s_reopen = true;
			}
		} else {
			svcSleepThread(20 * 1000000LL);
		}
		tick(osGetTime());
	}
}

/* Sockets do not survive sleep mode / the HOME Menu reliably: reopen them. */
static void on_apt(APT_HookType hook, void* param) {
	(void)param;
	if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) s_reopen = true;
}

bool net_init(void) {
	LightLock_Init(&s_lock);
	memset(&s_st, 0, sizeof(s_st));
	s_st.state = NET_SEARCHING;
	for (int i = 0; i < SLOT_COUNT; i++) {
		s_slots[i].state = SLOT_FREE;
		s_slots[i].data = (u8*)malloc(VR3DS_MAX_FRAME);
		if (!s_slots[i].data) return false;
	}
	s_rxWindow = osGetTime();
	aptHook(&s_hook, on_apt, NULL);
	s_running = true;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	/* above the main thread so the socket is drained while a frame decodes */
	s_thread = threadCreate(worker, NULL, 32 * 1024, prio - 1, -2, false);
	return s_thread != NULL;
}

void net_exit(void) {
	if (s_thread) {
		if (s_st.state == NET_STREAMING) send_bye();
		s_running = false;
		threadJoin(s_thread, U64_MAX);
		threadFree(s_thread);
		s_thread = NULL;
	}
	aptUnhook(&s_hook);
	close_socket();
	for (int i = 0; i < SLOT_COUNT; i++) {
		free(s_slots[i].data);
		s_slots[i].data = NULL;
	}
}

void net_status(NetStatus* out) {
	LightLock_Lock(&s_lock);
	*out = s_st;
	LightLock_Unlock(&s_lock);
}

bool net_is_streaming(void) {
	return s_st.state == NET_STREAMING;
}
