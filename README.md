# 3DS VR

> **This project was made entirely by an AI** (Claude, by Anthropic): the code, assets, build setup and documentation.
> **It has not been tested on real hardware yet.** Both halves build, but nobody has streamed a game to a 3DS with this
> version. Expect rough edges.

Use a Nintendo 3DS as a (very) small SteamVR headset. The 3DS shows the game on its top screen, in stereoscopic 3D if
you want it. Its gyroscope tracks your head (3DoF), and its buttons are your Xbox controller.

```
 PC                                                  Nintendo 3DS
 ┌──────────────────────────────┐   Wi-Fi (UDP)   ┌─────────────────────────────┐
 │ SteamVR ── 3DS VR driver     │ ─ video, sound ▶│ decoder ─ reprojection ─ 3D │
 │   virtual headset, encoder,  │                 │ top screen (2D or 3D)       │
 │   audio capture, Xbox pad    │ ◀ head, buttons │ gyroscope, buttons, touch   │
 │ 3DS VR app (setup, pairing)  │                 │                             │
 └──────────────────────────────┘                 └─────────────────────────────┘
```

## Features

- **Full-screen view, not split-screen.** Each eye gets the whole top screen. With the 3D slider up, the
  parallax barrier sends the left picture to your left eye and the right one to your right eye. The slider sets the
  depth, and at 0 the stream switches to a single 2D view.
- **Low-latency video with local reprojection.** Every frame carries the head pose it was rendered with. The console
  turns the picture to match where your head points *now*, so head rotation feels immediate even though the
  picture is a few frames old.
- **Hardware H.264 on New 3DS / New 2DS XL.** The Old 3DS family has no hardware decoder, so it gets JPEG frames
  decoded in software, at a lower resolution and frame rate.
- **3DoF head tracking** from the gyroscope and accelerometer (same filter as
  [SlimeVR 3DS](https://github.com/nicolas-riera/SlimeVR-3DS)), with a recenter button.
- **PC sound on the console** (speakers or headphones). There is no microphone and no virtual audio cable.
- **Xbox 360 controller.** All of the console's buttons, sticks and triggers. On the Old 3DS family the touch
  screen adds a right stick and ZL/ZR. On every model the touch screen has L3, R3 and the Xbox Guide button.
- **Model and battery passthrough.** SteamVR shows your exact console model and its status icons, and the
  console's battery level.
- **No configuration.** Run the app on the PC and the app on the 3DS, check the pairing code once, play.

## Supported consoles

| | New 3DS, New 3DS XL | New 2DS XL | 3DS, 3DS XL | 2DS |
|---|---|---|---|---|
| Video | H.264, 512×256 per eye, up to 60 fps | same | JPEG, 384×192 per eye, up to 30 fps | same |
| Stereoscopic 3D | yes (on by default) | no | yes (off by default) | no |
| Right stick | C-Stick | C-Stick | touch screen | touch screen |
| ZL / ZR | buttons | buttons | touch screen | touch screen |

## Requirements

- **Console:** custom firmware with the Homebrew Launcher, or FBI to install the CIA. For sound, the DSP firmware
  must be dumped (`sdmc:/3ds/dspfirm.cdc`, made by the [DSP1](https://github.com/zoogie/DSP1) homebrew).
- **PC:** Windows 10 or 11, SteamVR, and a GPU with Direct3D 11. NVIDIA, AMD and Intel hardware H.264 encoders are
  used through Media Foundation, and Windows' software encoder is the fallback.
- **Network:** the console and the PC on the same local network. The 3DS only has 2.4 GHz Wi-Fi, so keep it close
  to the access point.

## Install

Builds go in [Releases](https://github.com/nicolas-riera/3DS-VR/releases) when there are some. Until then, see
[Building](#building).

**Console:** copy `3DS-VR.3dsx` to `sdmc:/3ds/` and start it from the Homebrew Launcher. Or install `3DS-VR.cia`
with FBI to get a HOME Menu icon.

**PC:** unzip the `3DS VR` folder anywhere and run **`3DS VR.exe`**. On the first launch it:

1. registers its SteamVR driver (the `driver\3dsvr` folder next to it),
2. offers to open UDP port 39630 in the Windows Firewall (one click, one admin prompt),
3. offers to install [ViGEmBus](https://github.com/nefarius/ViGEmBus), the driver behind the virtual Xbox controller
   (one click, one admin prompt),
4. starts SteamVR.

The driver only activates while the app is running. Without the app, SteamVR behaves as if 3DS VR was not installed
and your other headsets work as usual. If SteamVR was already running when you opened the app, click
**Restart SteamVR**.

## Use

1. Run `3DS VR.exe` on the PC.
2. Start 3DS VR on the console. Put it down for two seconds so it can calibrate its gyroscope.
3. The console finds the PC by itself. The first time, it shows a 4-digit code and the app shows the same code: click
   **Pair**. The console is remembered.
4. Hold the console in front of your face and look straight ahead. The view recenters when the stream starts. Press
   **SELECT + START** or **Recenter** on the touch screen whenever you want.

### Controls

| 3DS | Xbox controller |
|---|---|
| B / A / Y / X | A / B / X / Y (Xbox positions). The *Face buttons* setting can match the letters instead. |
| L / R | LB / RB |
| ZL / ZR (New 3DS family) | LT / RT |
| Circle Pad | left stick |
| C-Stick (New 3DS family) | right stick |
| D-Pad | D-Pad |
| START / SELECT | Start / Back |
| touch screen: L3, R3, Guide | L3, R3, Guide (Guide opens the SteamVR dashboard) |
| touch screen, Old 3DS family: stick, ZL, ZR | right stick, LT, RT |
| SELECT + START, or touch screen: Recenter | recenter the view (not sent to the game) |

### Console settings

Open them with **Settings** (or **Menu** on the Old 3DS family) on the touch screen.

| Setting | What it does |
|---|---|
| Field of view | how wide the view is on the top screen (50° to 110°, 80° by default) |
| Your height | puts your head at the right height in SteamVR (120 to 220 cm). Lower it if you play seated. |
| Video quality | the bitrate (Low, Medium, High, Ultra). Lower it if the picture freezes or breaks up. |
| Frame rate | 30 or 60 fps (New 3DS family), 20 or 30 fps (Old 3DS family) |
| Stereoscopic 3D | streams both eyes when the 3D slider is up |
| Face buttons | Xbox positions or matching letters |
| Forget PCs | every PC will have to pair again |

Settings are saved in `sdmc:/3ds/3DS-VR/settings.ini`.

## How it works

**PC side** (`streamer/`):

- **SteamVR driver** (`driver_3dsvr.dll`). It is a headset with a *direct mode* component. SteamVR renders into
  textures the driver owns, and each frame's layers (game and overlays) arrive with the exact head pose they were
  rendered with. The driver composites them with Direct3D 11 straight at the stream resolution. The eyes are
  stacked vertically: 512×256 each for H.264, 384×192 for JPEG.
- **Encoder.** The picture is read back and encoded with the Media Foundation H.264 encoder: constrained baseline,
  low-latency mode, CBR, no B-frames, one slice, key frames on request. For the Old 3DS family, WIC encodes JPEG
  with the quality steered to the bitrate. Packets are paced so a key frame does not overflow the console's Wi-Fi
  buffers.
- **Field of view.** The rendered field of view is the console's field of view plus a margin, so that reprojection
  has picture to show at the edges. The driver changes the projection and the eye distance at runtime when you
  change the field of view or move the 3D slider.
- **Audio.** The default output device is captured with WASAPI loopback, resampled to 32 kHz and encoded to
  IMA-ADPCM (256 kbit/s).
- **Gamepad.** A virtual Xbox 360 controller is created with ViGEmBus while a console is connected.
- **Model and battery.** The console's model becomes the headset's model number, which selects that console's
  render model and status icons. The icons follow SteamVR's own style (see `driver.vrresources`). The battery
  level is reported as the headset's battery.

**Console side** (`console/`, C with libctru, citro3d and citro2d):

- **Network.** Discovery (HELLO broadcasts), pairing, reassembly of fragmented frames, and requests for a key frame
  when a frame is lost.
- **Decoding.** The MVD hardware decoder on the New 3DS family, libjpeg-turbo on the others. Either way the frame
  ends up in a 512×512 RGB565 texture.
- **Reprojection.** Each eye's picture is a quad on the plane z = −1 of the pose it was rendered with. The quad is
  drawn by a small PICA200 vertex shader with the rotation from that pose to the current one, so only the latency
  of the screen itself is left.
- **Tracking.** A 100 Hz thread runs the gyroscope and accelerometer through a Mahony filter, and sends the head
  orientation, the gamepad state, the battery and the 3D slider.
- **Recentering.** The sensors are in the bottom half of the console but you look at the top screen, and the hinge
  angle is unknown. Recentering assumes you look straight ahead: it solves the rotation around the hinge that makes
  the view level, keeps the roll from gravity, and removes the heading.

The wire protocol is described in [docs/protocol.md](docs/protocol.md) and defined in
[protocol/vr3ds_protocol.h](protocol/vr3ds_protocol.h).

## Troubleshooting

| Problem | Try |
|---|---|
| The console never finds the PC | Same network? Click **Allow** in the app (firewall). Some routers isolate Wi-Fi clients ("AP isolation"): turn that off. |
| The app says SteamVR runs without 3DS VR | Click **Restart SteamVR**. The driver only loads if SteamVR starts after the app. |
| The picture freezes or breaks up | Lower *Video quality* or *Frame rate*, get closer to the access point, use 2D. |
| Grey picture on New 3DS | It waits for a key frame. It should clear within a second, otherwise restart the stream. |
| No sound | The DSP firmware dump is missing (the console says so). Sound also plays on the PC: turn the PC speakers down. |
| No controller | Install ViGEmBus from the app. |
| The view drifts sideways | Without a magnetometer the heading drifts slowly: recenter. |

Logs: `%APPDATA%\3DS VR\driver.log` on the PC, and SteamVR's `vrserver.txt`.

## Building

**Console.** You need devkitPro with `3ds-dev` and `3ds-libjpeg-turbo`, plus `makerom` and `bannertool` in your
PATH (or in `/opt/ctrtools`).

```sh
cd console
./build.sh          # dist/3DS-VR.3dsx, .cia and .3ds
```

**PC.** You need Visual Studio 2022 (C++ desktop workload) and CMake 3.20 or newer. The OpenVR driver header and
ViGEmClient are downloaded at configure time.

```sh
cd streamer
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release   # build/dist/3DS VR/ is the folder to ship
```

**Assets.** The icons, banner, jingle, SteamVR status icons, render models and `driver.vrresources` come from one
script that uses only the standard library:

```sh
python3 tools/gen_assets.py
```

**Testing the PC side without a console.** `tools/fake_console.py` behaves like the console on the network. It
discovers the PC, pairs, sends head tracking, and receives the stream. It saves the H.264 stream (`stream.h264`, play
it with `ffplay`) or a few JPEG frames, and prints the frame rate, bitrate and losses.

```sh
python3 tools/fake_console.py --model new3dsxl --seconds 20   # or --model 3ds for the JPEG path
```

## Project layout

```
protocol/   wire protocol shared by both sides
console/    3DS app (source/, shaders/reproject.v.pica, meta/ for the icon, banner and RSF)
streamer/   PC side: driver/ (SteamVR driver and its resources), app/ (3DS VR.exe), common/ (driver <-> app link)
tools/      asset generator, fake console for testing
docs/       protocol description
```

## License

[MIT](LICENSE). Parts of the console's sensor fusion come from [SlimeVR 3DS](https://github.com/nicolas-riera/SlimeVR-3DS)
(MIT). ViGEmClient is MIT, and the OpenVR header is BSD-3-Clause. 3DS VR is not affiliated with Nintendo or Valve.
