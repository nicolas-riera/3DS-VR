// Private link between the SteamVR driver (the feeder) and the 3DS VR gamepad
// driver (gamepad_driver/). The feeder opens the device through its own
// interface and pushes XINPUT_GAMEPAD-like reports. The virtual Xbox 360
// controller is plugged in while a feeder holds the device open.
#pragma once
#include <cstdint>

#include <windows.h>
#include <winioctl.h>

// {F9F394FE-6D12-4153-BDDA-94F1F1C60DB7}
static const GUID GUID_DEVINTERFACE_VR3DS_PAD = {
	0xf9f394fe, 0x6d12, 0x4153, {0xbd, 0xda, 0x94, 0xf1, 0xf1, 0xc6, 0x0d, 0xb7}};

#define VR3DS_PAD_HARDWARE_ID L"Root\\3DSVR_Gamepad"
#define VR3DS_PAD_INF_NAME    L"vr3ds_gamepad.inf"

// in: Vr3dsPadReport. The first handle to send it becomes the feeder.
#define IOCTL_VR3DS_PAD_SET_STATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0xA00, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// out: Vr3dsPadFeedback (what games asked for)
#define IOCTL_VR3DS_PAD_GET_FEEDBACK CTL_CODE(FILE_DEVICE_UNKNOWN, 0xA01, METHOD_BUFFERED, FILE_READ_ACCESS)

#pragma pack(push, 1)

struct Vr3dsPadReport { // same layout as XINPUT_GAMEPAD
	uint16_t buttons;
	uint8_t leftTrigger, rightTrigger;
	int16_t lx, ly, rx, ry;
};

struct Vr3dsPadFeedback {
	uint8_t largeMotor, smallMotor; // 0-255
	uint8_t led;                    // Xbox 360 ring pattern, 6-9 = player 1-4
	uint8_t reserved;
};

#pragma pack(pop)
