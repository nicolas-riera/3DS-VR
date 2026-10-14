#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "mathx.h"
#include "vr3ds_protocol.h"

/*
 * Networking thread: discovery (HELLO broadcasts), pairing, session
 * handling, video frame reassembly and audio packets.
 */

typedef enum {
	NET_OFFLINE,   /* no Wi-Fi / sockets unavailable */
	NET_SEARCHING, /* broadcasting HELLO */
	NET_PAIRING,   /* a PC asked the user to confirm a code */
	NET_REJECTED,  /* the PC refused the pairing */
	NET_STREAMING,
} NetState;

typedef struct {
	NetState state;
	u16 pairCode;
	char pcName[VR3DS_NAME_LEN + 1];
	bool hasConfig;
	Vr3dsConfig config;
	u32 sessionId;
	u32 localIp;
	float rxKbps;
} NetStatus;

/* A complete video frame, owned by the caller until net_release_frame(). */
typedef struct {
	u32 frameId;
	u16 configId;
	u8 flags;
	u32 size;
	u64 timestampUs;
	quat pose;
	u64 recvTick;
	const u8* data;
	int slot;
} NetFrame;

bool net_init(void);
void net_exit(void);

void net_status(NetStatus* out);
bool net_is_streaming(void);

/* Oldest complete frame (latestOnly: newest one, older ones are dropped). */
bool net_pop_frame(NetFrame* out, bool latestOnly);
void net_release_frame(const NetFrame* f);
/* Frames that never completed or were dropped before decoding. */
u32 net_frames_lost(void);

void net_send_tracking(Vr3dsTracking* t);
void net_request_keyframe(u16 configId);
/* Sends SETTINGS right away (also sent every second). */
void net_settings_changed(void);
/* Ends the session (sends BYE) and searches again. */
void net_disconnect(void);
