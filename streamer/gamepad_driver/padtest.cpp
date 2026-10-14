// Gamepad driver test tool (not shipped).
//   padtest        lists the XUSB devices and what each XInput DLL reports
//   padtest feed   plugs the 3DS VR controller in and moves its left stick,
//                  as the SteamVR driver would (run "padtest" in a second window)
#include <windows.h>
#include <setupapi.h>
#include <xinput.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/vr3ds_pad.h"

#pragma comment(lib, "setupapi.lib")

static const GUID GUID_DEVINTERFACE_XUSB = {0xec87f1e3, 0xc13b, 0x4100, {0xb5, 0xf7, 0x8b, 0x84, 0xd5, 0x42, 0x60, 0xcb}};

static int ListInterfaces(const GUID& guid, const char* name) {
	HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) return 0;
	SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
	int n = 0;
	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd); i++, n++) {
		wchar_t buf[1024];
		auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
		detail->cbSize = sizeof(*detail);
		if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, sizeof(buf), nullptr, nullptr))
			printf("%s: %ls\n", name, detail->DevicePath);
	}
	SetupDiDestroyDeviceInfoList(set);
	if (!n) printf("%s: none\n", name);
	return n;
}

static HANDLE OpenPad() {
	HANDLE h = INVALID_HANDLE_VALUE;
	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_VR3DS_PAD, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) return h;
	SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
	wchar_t buf[1024];
	auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
	detail->cbSize = sizeof(*detail);
	if (SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_VR3DS_PAD, 0, &ifd) &&
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, sizeof(buf), nullptr, nullptr))
		h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
						OPEN_EXISTING, 0, nullptr);
	SetupDiDestroyDeviceInfoList(set);
	return h;
}

static int Feed() {
	HANDLE h = OpenPad();
	if (h == INVALID_HANDLE_VALUE) {
		printf("The 3DS VR gamepad driver is not running (error %lu)\n", GetLastError());
		return 1;
	}
	printf("Plugged in. The left stick turns, A blinks. Ctrl+C to unplug.\n");
	for (int t = 0;; t++) {
		Vr3dsPadReport r = {};
		r.lx = (int16_t)(30000 * cos(t * 0.05));
		r.ly = (int16_t)(30000 * sin(t * 0.05));
		if ((t / 50) & 1) r.buttons = XINPUT_GAMEPAD_A;
		DWORD n;
		if (!DeviceIoControl(h, IOCTL_VR3DS_PAD_SET_STATE, &r, sizeof(r), nullptr, 0, &n, nullptr)) {
			printf("SET_STATE failed (error %lu)\n", GetLastError());
			return 1;
		}
		Vr3dsPadFeedback fb = {};
		if (t % 50 == 0 && DeviceIoControl(h, IOCTL_VR3DS_PAD_GET_FEEDBACK, nullptr, 0, &fb, sizeof(fb), &n, nullptr))
			printf("rumble %3u %3u  LED %u\n", fb.largeMotor, fb.smallMotor, fb.led);
		Sleep(20);
	}
}

static void Poll(const wchar_t* dll) {
	HMODULE m = LoadLibraryW(dll);
	if (!m) {
		printf("%ls: not installed\n", dll);
		return;
	}
	auto getState = (decltype(&XInputGetState))GetProcAddress(m, "XInputGetState");
	auto getCaps = (decltype(&XInputGetCapabilities))GetProcAddress(m, "XInputGetCapabilities");
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
		XINPUT_STATE s = {};
		DWORD err = getState(i, &s);
		if (err != ERROR_SUCCESS) {
			printf("%ls: slot %lu %s\n", dll, i, err == ERROR_DEVICE_NOT_CONNECTED ? "empty" : "error");
			continue;
		}
		XINPUT_CAPABILITIES c = {};
		getCaps(i, 0, &c);
		printf("%ls: slot %lu connected (type %u/%u) packet %lu buttons %04X LX %6d LY %6d LT %3u RT %3u\n", dll, i,
			   c.Type, c.SubType, s.dwPacketNumber, s.Gamepad.wButtons, s.Gamepad.sThumbLX, s.Gamepad.sThumbLY,
			   s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger);
	}
	FreeLibrary(m);
}

int wmain(int argc, wchar_t** argv) {
	if (argc > 1 && !wcscmp(argv[1], L"feed")) return Feed();
	ListInterfaces(GUID_DEVINTERFACE_XUSB, "XUSB");
	ListInterfaces(GUID_DEVINTERFACE_VR3DS_PAD, "3DS VR feeder");
	const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
	for (const wchar_t* dll : dlls) Poll(dll);
	return 0;
}
