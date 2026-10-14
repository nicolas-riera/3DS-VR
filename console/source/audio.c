#include "audio.h"

#include <3ds.h>
#include <string.h>

#define CHANNEL      0
#define BUFFER_COUNT 10
#define PREBUFFER    3 /* packets queued before (re)starting playback: 48 ms of jitter */
#define MAX_QUEUED   7 /* above this the PC clock runs ahead: drop */

static bool s_ok;
static ndspWaveBuf s_wave[BUFFER_COUNT];
static s16* s_pcm;
static LightLock s_lock;
static bool s_primed;
static int s_pending; /* decoded, not yet given to the DSP while priming */
static int s_pendingIdx[BUFFER_COUNT];

static const int s_stepTable[89] = {
	7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
	31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
	130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
	544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
	2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
	9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int s_indexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static inline s16 ima_step(int nibble, int* pred, int* index) {
	int step = s_stepTable[*index];
	int diff = step >> 3;
	if (nibble & 1) diff += step >> 2;
	if (nibble & 2) diff += step >> 1;
	if (nibble & 4) diff += step;
	if (nibble & 8) diff = -diff;
	int p = *pred + diff;
	if (p > 32767) p = 32767;
	if (p < -32768) p = -32768;
	*pred = p;
	int i = *index + s_indexTable[nibble];
	*index = i < 0 ? 0 : (i > 88 ? 88 : i);
	return (s16)p;
}

bool audio_init(void) {
	LightLock_Init(&s_lock);
	if (R_FAILED(ndspInit())) return false;
	s_pcm = (s16*)linearAlloc(BUFFER_COUNT * VR3DS_AUDIO_FRAMES * 2 * sizeof(s16));
	if (!s_pcm) {
		ndspExit();
		return false;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(CHANNEL);
	ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetRate(CHANNEL, (float)VR3DS_AUDIO_RATE);
	ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {1.0f, 1.0f};
	ndspChnSetMix(CHANNEL, mix);
	memset(s_wave, 0, sizeof(s_wave));
	for (int i = 0; i < BUFFER_COUNT; i++) {
		s_wave[i].data_pcm16 = s_pcm + i * VR3DS_AUDIO_FRAMES * 2;
		s_wave[i].status = NDSP_WBUF_DONE;
	}
	s_ok = true;
	return true;
}

void audio_exit(void) {
	if (!s_ok) return;
	s_ok = false;
	ndspChnReset(CHANNEL);
	ndspExit();
	linearFree(s_pcm);
	s_pcm = NULL;
}

bool audio_available(void) {
	return s_ok;
}

static int queued_count(void) {
	int n = 0;
	for (int i = 0; i < BUFFER_COUNT; i++)
		if (s_wave[i].status == NDSP_WBUF_QUEUED || s_wave[i].status == NDSP_WBUF_PLAYING) n++;
	return n;
}

static bool is_pending(int idx) {
	for (int i = 0; i < s_pending; i++)
		if (s_pendingIdx[i] == idx) return true;
	return false;
}

void audio_submit(const Vr3dsAudioHeader* hdr, const uint8_t* data) {
	if (!s_ok || hdr->frames == 0 || hdr->frames > VR3DS_AUDIO_FRAMES) return;
	LightLock_Lock(&s_lock);

	int queued = queued_count();
	if (queued == 0 && s_primed) {
		/* underrun: build some margin again before restarting */
		s_primed = false;
	}
	if (queued >= MAX_QUEUED) {
		LightLock_Unlock(&s_lock);
		return;
	}

	int idx = -1;
	for (int i = 0; i < BUFFER_COUNT; i++) {
		if ((s_wave[i].status == NDSP_WBUF_DONE || s_wave[i].status == NDSP_WBUF_FREE) && !is_pending(i)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		LightLock_Unlock(&s_lock);
		return;
	}

	s16* out = (s16*)s_wave[idx].data_pcm16;
	int pl = hdr->predictor[0], pr = hdr->predictor[1];
	int il = hdr->stepIndex[0] > 88 ? 88 : hdr->stepIndex[0];
	int ir = hdr->stepIndex[1] > 88 ? 88 : hdr->stepIndex[1];
	for (int i = 0; i < hdr->frames; i++) {
		u8 b = data[i];
		out[i * 2] = ima_step(b & 0x0F, &pl, &il);
		out[i * 2 + 1] = ima_step(b >> 4, &pr, &ir);
	}
	s_wave[idx].nsamples = hdr->frames;
	DSP_FlushDataCache(out, hdr->frames * 2 * sizeof(s16));

	if (s_primed) {
		ndspChnWaveBufAdd(CHANNEL, &s_wave[idx]);
	} else {
		s_pendingIdx[s_pending++] = idx;
		if (s_pending >= PREBUFFER) {
			for (int i = 0; i < s_pending; i++) ndspChnWaveBufAdd(CHANNEL, &s_wave[s_pendingIdx[i]]);
			s_pending = 0;
			s_primed = true;
		}
	}
	LightLock_Unlock(&s_lock);
}

void audio_reset(void) {
	if (!s_ok) return;
	LightLock_Lock(&s_lock);
	ndspChnWaveBufClear(CHANNEL);
	for (int i = 0; i < BUFFER_COUNT; i++) s_wave[i].status = NDSP_WBUF_DONE;
	s_pending = 0;
	s_primed = false;
	LightLock_Unlock(&s_lock);
}
