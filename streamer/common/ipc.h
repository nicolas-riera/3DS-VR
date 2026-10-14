// Local link between the SteamVR driver (inside vrserver.exe) and the 3DS VR
// app: UDP on the loopback interface, fixed-size structs.
#pragma once
#include <cstdint>

#define VR3DS_IPC_APP_PORT    39632 // the app listens (status from the driver)
#define VR3DS_IPC_DRIVER_PORT 39633 // the driver listens (commands from the app)
#define VR3DS_IPC_MAGIC       0x43504933u

// Named mutex held by the app while it runs. The driver only activates when
// it exists, so SteamVR behaves as if 3DS VR was not installed otherwise.
#define VR3DS_APP_MUTEX L"Local\\3DSVR_App_Running"

enum IpcType : uint8_t {
	IPC_STATUS = 1,        // driver -> app, twice a second and on changes
	IPC_PAIR_DECISION = 2, // app -> driver
	IPC_FORGET_ALL = 3,    // app -> driver
};

enum IpcState : uint8_t {
	IPC_STATE_WAITING = 0,   // no console
	IPC_STATE_STREAMING = 1,
};

enum IpcGamepad : uint8_t {
	IPC_PAD_IDLE = 0,
	IPC_PAD_OK = 1,
	IPC_PAD_NO_DRIVER = 2, // ViGEmBus is not installed
	IPC_PAD_ERROR = 3,
};

#pragma pack(push, 1)

struct IpcStatus {
	uint32_t magic;
	uint8_t type;
	uint8_t state;        // IpcState
	uint8_t model;        // Vr3dsModel
	uint8_t codec;        // Vr3dsCodec
	uint8_t stereo;
	uint8_t battery;      // percent
	uint8_t batteryFlags; // VR3DS_BATT_*
	uint8_t gamepad;      // IpcGamepad
	uint8_t audio;        // 1 when loopback capture runs
	uint8_t pairPending;  // a console waits for the user's decision
	uint8_t pairModel;
	uint8_t reserved;
	uint16_t pairCode;
	uint16_t eyeWidth, eyeHeight;
	uint16_t fps;         // target frame rate
	uint16_t bitrateKbps; // target bitrate
	uint64_t pairConsoleId;
	float encodeFps;      // frames sent per second
	float sentMbps;
	float latencyMs;      // capture -> on the console screen -> back
	float consoleFps;     // frames decoded by the console per second
	uint32_t framesDropped;
	uint32_t pairedCount;
	char consoleName[32];
	char consoleIp[16];
	char pairName[32];
	char pairIp[16];
	char encoder[48];
};

struct IpcPairDecision {
	uint32_t magic;
	uint8_t type;
	uint8_t accept;
	uint8_t reserved[2];
	uint64_t consoleId;
};

struct IpcCommand {
	uint32_t magic;
	uint8_t type;
	uint8_t reserved[3];
};

#pragma pack(pop)
