# 3DS VR

> **This project was made entirely by an AI** (Claude, by Anthropic): the code, assets, build setup and documentation.
> **It has only been tested on a New Nintendo 3DS XL.** The other models (New 3DS, New 2DS XL, and the Old 3DS
> family with its software JPEG decoding) should work but nobody has tried them. Expect rough edges.

Use a Nintendo 3DS as a (very) small SteamVR headset. The 3DS shows the game on its top screen, in stereoscopic 3D if
you want it. Its gyroscope tracks your head (3DoF), and its buttons are your Xbox controller, or two VR controllers.

```
 PC                                                  Nintendo 3DS
 ┌──────────────────────────────┐   Wi-Fi (UDP)   ┌─────────────────────────────┐
 │ SteamVR ── 3DS VR driver     │ ─ video, sound ▶│ decoder ─ reprojection ─ 3D │
 │   virtual headset, encoder,  │                 │ top screen (2D or 3D)       │
 │   audio capture, controllers │ ◀ head, buttons │ gyroscope, buttons, touch   │
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
- **Two ways to use the buttons.** An Xbox 360 controller (the default), or two VR controllers for games that need
  motion controllers. On the Old 3DS family the touch screen adds a right stick and ZL/ZR.
- **SteamVR menu with the console.** Guide (touch screen) opens SteamVR's dashboard; aim with your head and click.
- **Model and battery passthrough.** SteamVR shows your exact console model and its status icons, and the
  console's battery level.
- **No configuration.** Run the app on the PC and the app on the 3DS, check the pairing code once, play.

## Supported consoles

| | New 3DS, New 3DS XL | New 2DS XL | 3DS, 3DS XL | 2DS |
|---|---|---|---|---|
| Tested | New 3DS XL only | no | no | no |
| Video | H.264, 512×256 per eye, up to 60 fps | same | JPEG, 384×192 per eye, up to 30 fps | same |
| Stereoscopic 3D | yes (on by default) | no | yes (off by default) | no |
| Right stick | C-Stick | C-Stick | touch screen | touch screen |
| ZL / ZR | buttons | buttons | touch screen | touch screen |

## Requirements

- **Console:** custom firmware with the Homebrew Launcher, or FBI to install the CIA. For sound, the DSP firmware
  must be dumped (`sdmc:/3ds/dspfirm.cdc`, made by the [DSP1](https://github.com/zoogie/DSP1) homebrew).
- **PC:** Windows 10 (1803 or later) or 11, SteamVR, and a GPU with Direct3D 11. NVIDIA, AMD and Intel hardware
  H.264 encoders are used through Media Foundation, and Windows' software encoder is the fallback.
- **Network:** the console and the PC on the same local network. The 3DS only has 2.4 GHz Wi-Fi, so keep it close
  to the access point.

## Install

Download the latest [release](https://github.com/nicolas-riera/3DS-VR/releases/latest), or see
[Building](#building).

**Console:** copy `3DS-VR.3dsx` to `sdmc:/3ds/` and start it from the Homebrew Launcher. Or install `3DS-VR.cia`
with FBI to get a HOME Menu icon.

**PC:** unzip `3DS-VR-PC.zip` anywhere (keep the folder together) and run **`3DS VR.exe`**. It sets everything up
itself, one click at a time:

1. it registers its SteamVR driver (the `driver\3dsvr` folder next to it),
2. it offers to open UDP port 39630 in the Windows Firewall (one click, one admin prompt),
3. it offers to install the 3DS VR gamepad driver, the driver behind the virtual Xbox controller (one click, one admin
   prompt, see [Gamepad driver](#gamepad-driver)). If [ViGEmBus](https://github.com/nefarius/ViGEmBus) is already
   installed, it works with that instead and there is nothing to install,
4. it starts SteamVR.

The app registers the driver with SteamVR when it starts and removes it when it closes, so without the app SteamVR
behaves as if 3DS VR was not installed and your other headsets work as usual. If SteamVR was already running when
you opened the app, click **Restart SteamVR**.

## Use

1. Run `3DS VR.exe` on the PC.
2. Start 3DS VR on the console. Put it down for two seconds so it can calibrate its gyroscope.
3. The console finds the PC by itself. The first time, it shows a 4-digit code and the app shows the same code: click
   **Pair**. The console is remembered.
4. Hold the console in front of your face and look straight ahead. The view recenters when the stream starts. Press
   **SELECT + START** or **Recenter** on the touch screen whenever you want.

### The PC app

The window lists what 3DS VR needs, each with its state and, when something is missing, the button that fixes it.

| Row | What it says |
|---|---|
| SteamVR | Running with the 3DS VR headset, starting, not running, or running without 3DS VR (**Restart SteamVR**). |
| Network | Whether the firewall lets consoles in (**Allow** adds the rule). |
| Xbox controller | Whether the virtual controller is ready, connected, or off because the console is in VR controllers mode. **Install** / **Remove** manage the gamepad driver (**Remove** needs a second click). |
| Sound | Whether the PC's sound is captured for the console. |

While a console streams, the top card shows its model, nickname and battery, the stream format (2D or 3D, size per
eye, codec, frame rate), the frame rate and bitrate actually sent, the latency (from rendering on the PC to the
picture on the console's screen) and the encoder in use. When a new console asks to pair, the card shows its code
with **Pair** and **Refuse**. **Forget all** removes every paired console (click twice).

### The console app

Before streaming, the top screen shows what the console is doing (connecting to Wi-Fi, looking for the PC, pairing)
and the bottom screen has **Settings** and **Exit**. The console takes the Wi-Fi for itself while the app runs
(StreetPass and SpotPass are paused) and reconnects to your saved access point by itself if the connection drops.

While streaming, the touch screen has the buttons the console does not have:

| New 3DS family | Old 3DS family |
|---|---|
| **L3**, **Guide**, **R3** (top row), **Recenter**, **Settings** | **ZL** and **ZR** (top corners), **L3**, **R3**, **Guide**, **Recenter**, **Menu** (left side), and a **stick** on the right side that you drag with the stylus or a thumb (the right stick) |

**L3**, **R3**, **Guide**, **ZL**, **ZR** and the stick are held as long as you touch them. While the settings are
open the console sends no buttons to the PC.

The **3D slider** does two things: anywhere above 0 (with *Stereoscopic 3D* on) the stream carries both eyes, and its
position sets the depth (the distance between the two eyes in SteamVR, up to 64 mm). At 0 the stream switches to one
centred view after half a second, which halves the picture the console has to decode.

### Xbox controller mode

The default. The console is an Xbox 360 controller for the PC: games that play with a controller see it like any
other, while a console is streaming.

| 3DS | Xbox controller |
|---|---|
| B / A / Y / X | A / B / X / Y (same positions). With *Face buttons* set to *Same letters*: A / B / X / Y. |
| L / R | LB / RB |
| ZL / ZR (Old 3DS family: touch screen) | LT / RT |
| Circle Pad | left stick |
| C-Stick (Old 3DS family: touch-screen stick) | right stick |
| D-Pad | D-Pad |
| START / SELECT | Start / Back |
| touch screen: L3, R3 | L3, R3 |
| touch screen: Guide | opens or closes SteamVR's dashboard (not sent to the game, where it would open the Xbox Game Bar) |
| SELECT + START, or touch screen: Recenter | recenter the view (not sent to the game) |

### SteamVR's dashboard

**Guide** on the touch screen opens and closes SteamVR's dashboard in both modes. The pointer follows your head.

| 3DS | In the dashboard (Xbox controller mode) |
|---|---|
| A or ZR | click |
| ZL | right-click |
| B | back |
| Y | home |
| D-Pad | move between items |
| C-Stick (or the touch-screen stick) | scroll |

While the dashboard is open the game gets no input from the console. In VR controllers mode the dashboard is used
like with real controllers: the right controller points where you look, and its trigger (ZR) clicks.

### VR controllers mode

For games that need motion controllers. Set **Buttons on the PC** to **VR controllers** in the console settings (it
switches at once, even while streaming): the console becomes two Quest 2 (Touch) controllers, which every SteamVR
game has bindings for. The Xbox controller is unplugged meanwhile, so games do not get every button twice.

The two controllers have no tracking of their own: they are held low in front of you (12 cm to each side, 14 cm
down, 32 cm ahead of your eyes) and turn with your head, both pointing where the console looks. So you aim with your
head, and the game sees your hands follow your view.

| 3DS | Left controller | Right controller |
|---|---|---|
| Circle Pad | stick | |
| D-Pad (when the Circle Pad is centred) | stick (full tilt) | |
| C-Stick (Old 3DS family: touch-screen stick) | | stick |
| ZL / ZR (Old 3DS family: touch screen) | trigger | trigger |
| L / R | grip | grip |
| Y / X | X / Y | |
| B / A | | A / B |
| START | menu | |
| touch screen: L3 / R3 | stick click | stick click |
| touch screen: Guide | SteamVR dashboard | SteamVR dashboard |

With *Face buttons* set to *Same letters*, A and B swap (3DS A is Touch A) and so do X and Y. SELECT has no
controller button (it is still half of SELECT + START, which recenters). The buttons can be remapped per game in
SteamVR's controller bindings (*Settings* → *Controllers* → *Manage controller bindings*), like real Touch
controllers. The hands do not move their fingers: there is no finger tracking.

### Console settings

Open them with **Settings** (or **Menu** on the Old 3DS family) on the touch screen. Hold **+** or **−** to change
a value quickly.

| Setting | Values | What it does |
|---|---|---|
| Field of view | 50° to 110°, 80° by default | How wide the game's view is on the top screen. Wider shows more but makes everything smaller. The PC renders a little more than this, so that reprojection has picture to show at the edges. |
| Your height | 120 to 220 cm (with feet and inches), 170 cm by default | Puts your eyes at the right height in SteamVR. Lower it if you play seated. |
| Video quality | Low, Medium (default), High, Ultra | The bitrate, shown next to it: 2, 3.5, 5 or 7 Mbps for H.264 (New 3DS family), 1.5, 2.5, 3.5 or 5 Mbps for JPEG (Old 3DS family). Lower it if the picture freezes or breaks up. |
| Frame rate | 30 or 60 fps (New 3DS family, 60 by default), 20 or 30 fps (Old 3DS family, 30 by default) | How many pictures per second the PC sends. |
| Stereoscopic 3D | On, Off (models with a 3D screen) | Whether the 3D slider can turn the stream to 3D. On by default on the New 3DS family, off on the Old 3DS family (software decoding of two eyes is slow). |
| Face buttons | Xbox positions (default), Same letters | Xbox positions maps by position (3DS B, at the bottom, is Xbox A). Same letters maps by name (3DS A is Xbox A). |
| Buttons on the PC | Xbox controller (default), VR controllers | See [Xbox controller mode](#xbox-controller-mode) and [VR controllers mode](#vr-controllers-mode). |

At the bottom: **Back** closes the settings, **Forget PCs** (tap twice) makes every PC pair again, and **Exit** quits
the app. Settings are saved in `sdmc:/3ds/3DS-VR/settings.ini` a second after the last change.

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
- **Gamepad.** A virtual Xbox 360 controller is plugged in while a console is connected, through the 3DS VR
  gamepad driver (below), or through ViGEmBus when only that is installed. Guide is the headset's system button,
  which toggles SteamVR's dashboard. While the dashboard is open, the SteamVR driver also connects the console's
  buttons as a SteamVR device of the `gamepad` type, which the dashboard knows how to use (it aims with the head).
  The rest of the time that device is disconnected, so games do not take it for a tracker or get every button
  twice. The bindings are in `resources/input/`.
- **VR controllers.** Two more SteamVR devices, a left and a right Quest 2 (Touch) controller: Touch's controller type,
  input profile and render model are SteamVR's own (from its Oculus driver), so games use their Touch bindings. Each
  pose is the head's pose plus a fixed offset, tilted up by the 38° between the Touch model's raw pose and its aim
  pose. They are only connected in VR controllers mode.
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

### Gamepad driver

`streamer/gamepad_driver/` is a small **user-mode** driver (UMDF 2, `vr3ds_gamepad.dll`) on a root-enumerated
device, `Root\3DSVR_Gamepad`. Games find Xbox controllers through XInput, which enumerates the *XUSB* device
interface and asks each device for its state with a few IOCTLs. The driver registers that interface and answers like
a wired Xbox 360 controller: information, capabilities, state, vibration, LED, battery, Guide button wait. It
supports the protocol of `xinput1_4` (Windows 8 and later), `xinput1_3` (most older games) and `xinput9_1_0`. The
SteamVR driver feeds it through a second, private interface. The controller only exists while it holds the device
open, so games see it plugged in when a console streams and unplugged otherwise.

Windows only loads kernel drivers signed by Microsoft, but a user-mode driver only needs a driver package signed by
a publisher the PC trusts. The project has no such certificate, so **the app makes one on your PC**, in one elevated
step:

1. it copies the package to a folder only administrators can write to, and checks that it is the exact package the
   app was built with,
2. it creates a code-signing certificate, *3DS VR Gamepad Driver (made on this PC)*, whose private key only exists
   in memory,
3. it writes the package's catalog (`vr3ds_gamepad.cat`) and signs it with that key,
4. it adds the certificate (public part only) to the local machine's *Trusted Root Certification Authorities* and
   *Trusted Publishers* stores, then drops the key: nothing else can ever be signed with it,
5. it creates the device and installs the driver on it.

So the driver is self-signed, and your PC trusts that one certificate. No private key ships with 3DS VR or stays on
your PC. The certificate is per machine: another PC gets its own. **Remove** (next to the Xbox controller in the app,
click twice) deletes the device, the driver package and the certificate. The details of each step are logged in
`%APPDATA%\3DS VR\gamepad_setup.log`.

The XUSB IOCTLs are not documented. Their codes and layouts come from public reverse engineering of the XInput DLLs,
mainly [OpenXinput](https://github.com/Nemirtingas/OpenXinput). The driver is written from scratch, no code was copied.

## Troubleshooting

| Problem | Try |
|---|---|
| The console never finds the PC | Same network? Click **Allow** in the app (firewall). Some routers isolate Wi-Fi clients ("AP isolation"): turn that off. |
| The app says SteamVR runs without 3DS VR | Click **Restart SteamVR**. The driver only loads if SteamVR starts after the app. |
| The picture freezes or breaks up | Lower *Video quality* or *Frame rate*, get closer to the access point, use 2D. |
| Grey picture on New 3DS | It waits for a key frame. It should clear within a second, otherwise restart the stream. |
| No sound | The DSP firmware dump is missing (the console says so). Sound also plays on the PC: turn the PC speakers down. |
| No controller | Click **Install** next to *Xbox controller* in the app. Games only see it while a console streams. `padtest.exe` (see [Building](#building)) lists what each XInput DLL sees. |
| The gamepad driver does not start | Look at *3DS VR Gamepad* in Device Manager (System devices) and at `%APPDATA%\3DS VR\gamepad_setup.log`. Some security policies only allow drivers signed by Microsoft (Smart App Control, S mode, company policies): use ViGEmBus there. Click **Remove**, then **Install** to start over. |
| "Restart Windows to finish" | Removing or updating the driver while SteamVR uses it needs a restart. Close SteamVR first to avoid it. |
| The view drifts sideways | Without a magnetometer the heading drifts slowly: recenter. |
| A game ignores the console's buttons | Games that only take motion controllers need *Buttons on the PC* set to *VR controllers*; games that only take a gamepad need *Xbox controller*. |
| SteamVR asks for Room Setup | It should not: the driver gives SteamVR its own play area. Restart SteamVR with the app running. |

Logs: `%APPDATA%\3DS VR\driver.log` (and `driver.previous.log`, the session before) and `gamepad_setup.log`
on the PC, and SteamVR's `vrserver.txt`.

## Building

**Console.** You need devkitPro with `3ds-dev` and `3ds-libjpeg-turbo`, plus `makerom` and `bannertool` in your
PATH (or in `/opt/ctrtools`).

```sh
cd console
./build.sh          # dist/3DS-VR.3dsx, .cia and .3ds
```

**PC.** You need Visual Studio 2022 (C++ desktop workload) and CMake 3.20 or newer. The OpenVR driver header,
ViGEmClient and the UMDF headers of the WDK are downloaded at configure time. The WDK comes from NuGet
(`Microsoft.Windows.WDK.x64`, a 106 MB download, only the few UMDF files are kept). `-DVR3DS_GAMEPAD_DRIVER=OFF`
builds without the gamepad driver (ViGEmBus only).

```sh
cd streamer
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release   # build/dist/3DS VR/ is the folder to ship
```

`build/Release/padtest.exe` tests the gamepad driver: without arguments it lists the XUSB devices and what each XInput
DLL reports, `padtest feed` plugs the controller in and moves its left stick, like the SteamVR driver would.

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
streamer/   PC side: driver/ (SteamVR driver and its resources), app/ (3DS VR.exe), common/ (driver <-> app link),
            gamepad_driver/ (virtual Xbox 360 controller driver and its INF, padtest)
tools/      asset generator, fake console for testing
docs/       protocol description
```

## License

[MIT](LICENSE). Parts of the console's sensor fusion come from [SlimeVR 3DS](https://github.com/nicolas-riera/SlimeVR-3DS)
(MIT). ViGEmClient is MIT, and the OpenVR header is BSD-3-Clause. The gamepad driver is built with the UMDF headers
and stub library of Microsoft's Windows Driver Kit (downloaded at build time, under the WDK license), and its XUSB
protocol follows the reverse engineering published by [OpenXinput](https://github.com/Nemirtingas/OpenXinput) (used
as documentation only). 3DS VR is not affiliated with Nintendo, Valve or Microsoft.
