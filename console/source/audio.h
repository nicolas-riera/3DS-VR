#pragma once
#include <stdbool.h>

#include "vr3ds_protocol.h"

/*
 * Game audio: IMA-ADPCM stereo at 32 kHz, decoded on reception and queued on
 * an NDSP channel. Needs the DSP firmware dump (sdmc:/3ds/dspfirm.cdc).
 */
bool audio_init(void);
void audio_exit(void);
bool audio_available(void);

/* Called from the network thread. `data` holds hdr->frames bytes. */
void audio_submit(const Vr3dsAudioHeader* hdr, const uint8_t* data);
/* Drops everything queued (end of session). */
void audio_reset(void);
