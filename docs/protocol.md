# 3DS VR protocol

All traffic is UDP, little-endian, and every struct is packed. The definitions live in
[`protocol/vr3ds_protocol.h`](../protocol/vr3ds_protocol.h), which both sides include.

| Port | Who listens |
|---|---|
| 39630 | PC (SteamVR driver) |
| 39631 | console |
| 39632 | 3DS VR app, loopback only (status from the driver) |
| 39633 | SteamVR driver, loopback only (commands from the app) |

Every packet starts with an 8-byte header: magic `0x52564433`, protocol version (1), packet type, then two reserved
bytes. A packet with the wrong magic or version is ignored.

## Session

```
console                                         PC
   |-- HELLO (broadcast + last PC, 2 Hz) -------->|
   |<------------- PAIR_STATUS pending, code -----|   unknown console: the app shows the code
   |                                    (user clicks Pair)
   |<---------------------------------- CONFIG ---|   known console: session starts
   |-- TRACKING (100 Hz) ------------------------>|
   |-- SETTINGS (on change and 1 Hz) ------------>|
   |<-------------------- VIDEO fragments, AUDIO -|
   |<------------------------- CONFIG (1 Hz) -----|
   |-- KEYFRAME_REQ (after a loss) -------------->|
   |<------------------------------- BYE ---------|   either side, or 3 s of silence
```

- **HELLO** carries a random console id and a secret token, both generated on the console's first launch and
  stored in `sdmc:/3ds/3DS-VR/identity.txt`. It also carries the model, the capabilities (hardware H.264,
  stereoscopic screen), the owner's nickname and the wanted stream settings.
- **Pairing.** The PC accepts a console when its id and token are in `%APPDATA%\3DS VR\paired_consoles.txt`.
  Otherwise it answers with a 4-digit code that both screens show, and adds the console when the user clicks
  **Pair**. *Forget PCs* on the console makes a new token, so every PC has to pair again.
- **CONFIG** fixes the stream format: codec, mono or stereo, eye size, frame rate, bitrate, and the field-of-view
  tangents covered by each encoded eye. `configId` changes whenever any of these change. The console then drops
  in-flight frames of the old config and waits for a key frame.
- **SETTINGS** is what the console wants: stereo on/off (3D setting and slider), frame rate, bitrate, the visible
  field of view, the player's height and what the buttons are on the PC (Xbox controller or VR controllers). The PC applies changes 250 ms after the last one, so that holding + / −
  does not restart the encoder at every step.

## Video

- A frame is split into fragments of at most 1400 bytes: the 48-byte `Vr3dsVideoHeader` followed by up to
  1352 bytes of data.
- The header repeats the frame id, the config id, the fragment index and count, the total size, the key-frame
  flag, the PC timestamp, and the head orientation the frame was rendered with.
- Fragment *i* holds bytes `[i × 1352, …)` of the frame.
- Frame ids are consecutive within a config. A frame the PC encoded but could not send still uses up an id, so a
  gap always means a missing reference.
- **H.264:** Annex-B, constrained baseline, one slice, SPS/PPS before every IDR. The console feeds MVD one NAL at a
  time. After a gap it drops P-frames and sends KEYFRAME_REQ (at most every 150 ms) until an IDR arrives.
- **JPEG:** every frame stands alone. The console always decodes the newest complete frame.
- **Layout:** eyes are stacked vertically, left eye on top. A mono stream has one eye, rendered with an eye
  distance of 0.

## Audio

Each AUDIO packet holds 512 stereo frames at 32 kHz (16 ms) of IMA-ADPCM. One byte per frame: the low nibble is the
left channel, the high nibble the right one. The header carries the ADPCM state (predictor and step index per
channel) at the start of the packet, so every packet decodes on its own and a lost packet costs 16 ms of silence.
The console keeps 48 ms queued before (re)starting playback.

## Tracking

TRACKING carries:

- the head orientation, already recentered, in SteamVR's head axes (+X right, +Y up, −Z forward);
- the Xbox controller state, using the XInput button bits and stick ranges;
- the 3D slider position, which the PC turns into the eye distance (0 to 64 mm);
- the battery level and charging flags, and the Wi-Fi strength;
- statistics: the last frame shown, with its PC timestamp echoed back for the latency estimate, and the frames
  decoded and dropped.

The PC reports the orientation to SteamVR with zero velocity, so SteamVR renders exactly that pose. The PC also
keeps a history of the poses it reported. It matches each frame's render pose against that history before sending
the frame, so the console gets back exactly the quaternion it sent.
