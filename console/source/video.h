#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#include "mathx.h"
#include "vr3ds_protocol.h"

/*
 * Video decoding: H.264 on the New 3DS hardware decoder (MVD), baseline JPEG
 * in software (libjpeg-turbo) on the Old 3DS family. Decoded frames land in a
 * 512x512 RGB565 texture, eyes stacked vertically (left on top).
 */

#define VIDEO_TEX_SIZE 512

typedef struct {
	C3D_Tex* tex;
	quat pose;         /* head orientation the frame was rendered with */
	bool stereo;
	float encTanX, encTanY;
	float u1;          /* right edge of the eye images in the texture */
	float vLeft[2];    /* {bottom, top} texture v of the left eye */
	float vRight[2];
} VideoView;

typedef struct {
	u32 shownFrameId;
	u64 shownFrameTs;
	u32 shownFrameAgeUs;
	u32 framesDecoded;
	u32 framesDropped;
	float fps;         /* decoded frames per second */
	float decodeMs;    /* average decode + upload time */
} VideoStats;

bool video_init(void);
void video_exit(void);

/* Decodes what the network received. Call once per main-loop iteration while streaming. */
void video_update(const Vr3dsConfig* cfg);
/* Forgets the current picture (end of session). */
void video_reset(void);

/* false until a first frame of the current config was decoded */
bool video_view(VideoView* out);
/* Thread-safe. */
void video_stats(VideoStats* out);
