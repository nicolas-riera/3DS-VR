#include "gamepad.h"

#include <windows.h>
#include <setupapi.h>

#include <ViGEm/Client.h>
#include <cstring>

#include "ipc.h"
#include "log.h"
#include "vr3ds_pad.h"

// Opens the 3DS VR gamepad driver and plugs the controller in (it stays
// plugged in while the handle is open).
bool Gamepad::ConnectPadDriver() {
	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_VR3DS_PAD, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) return false;
	HANDLE h = INVALID_HANDLE_VALUE;
	SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
	alignas(8) wchar_t buf[512];
	auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
	detail->cbSize = sizeof(*detail);
	if (SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_VR3DS_PAD, 0, &ifd) &&
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, sizeof(buf), nullptr, nullptr))
		h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
						OPEN_EXISTING, 0, nullptr);
	SetupDiDestroyDeviceInfoList(set);
	if (h == INVALID_HANDLE_VALUE) return false;
	Vr3dsPadReport idle = {};
	DWORD n;
	if (!DeviceIoControl(h, IOCTL_VR3DS_PAD_SET_STATE, &idle, sizeof(idle), nullptr, 0, &n, nullptr)) {
		Log("3DS VR gamepad driver: could not plug the controller in (error %lu)", GetLastError());
		CloseHandle(h);
		return false;
	}
	m_pad = h;
	return true;
}

uint8_t Gamepad::ConnectVigem() {
	PVIGEM_CLIENT client = vigem_alloc();
	if (!client) return IPC_PAD_ERROR;
	VIGEM_ERROR err = vigem_connect(client);
	if (!VIGEM_SUCCESS(err)) {
		vigem_free(client);
		Log("No 3DS VR gamepad driver, no ViGEmBus (0x%08X): no controller", (unsigned)err);
		return err == VIGEM_ERROR_BUS_NOT_FOUND ? IPC_PAD_NO_DRIVER : IPC_PAD_ERROR;
	}
	PVIGEM_TARGET target = vigem_target_x360_alloc();
	err = vigem_target_add(client, target);
	if (!VIGEM_SUCCESS(err)) {
		vigem_target_free(target);
		vigem_disconnect(client);
		vigem_free(client);
		Log("Could not plug the virtual Xbox 360 controller in (0x%08X)", (unsigned)err);
		return IPC_PAD_ERROR;
	}
	m_client = client;
	m_target = target;
	return IPC_PAD_OK;
}

uint8_t Gamepad::Connect() {
	if (m_pad || m_target) return m_status;
	m_hasLast = false;
	if (ConnectPadDriver()) {
		Log("Virtual Xbox 360 controller plugged in (3DS VR gamepad driver)");
		return m_status = IPC_PAD_OK;
	}
	m_status = ConnectVigem();
	if (m_status == IPC_PAD_OK) Log("Virtual Xbox 360 controller plugged in (ViGEmBus)");
	return m_status;
}

void Gamepad::Disconnect() {
	if (m_pad) {
		CloseHandle(m_pad); // unplugs it
		m_pad = nullptr;
	} else if (m_target) {
		PVIGEM_CLIENT client = (PVIGEM_CLIENT)m_client;
		PVIGEM_TARGET target = (PVIGEM_TARGET)m_target;
		vigem_target_remove(client, target);
		vigem_target_free(target);
		vigem_disconnect(client);
		vigem_free(client);
		m_client = m_target = nullptr;
	} else {
		return;
	}
	m_status = IPC_PAD_IDLE;
	Log("Virtual Xbox 360 controller unplugged");
}

// Rumble is not read back: the 3DS has no motor.
void Gamepad::Update(const Vr3dsGamepad& pad) {
	if (!m_pad && !m_target) return;
	if (m_hasLast && memcmp(&pad, &m_last, sizeof(pad)) == 0) return;
	m_last = pad;
	m_hasLast = true;
	if (m_pad) {
		Vr3dsPadReport r = {pad.buttons, pad.leftTrigger, pad.rightTrigger, pad.lx, pad.ly, pad.rx, pad.ry};
		DWORD n;
		if (!DeviceIoControl(m_pad, IOCTL_VR3DS_PAD_SET_STATE, &r, sizeof(r), nullptr, 0, &n, nullptr)) {
			Log("3DS VR gamepad driver stopped (error %lu)", GetLastError());
			CloseHandle(m_pad);
			m_pad = nullptr;
			m_status = IPC_PAD_ERROR;
		}
		return;
	}
	XUSB_REPORT r;
	XUSB_REPORT_INIT(&r);
	r.wButtons = pad.buttons;
	r.bLeftTrigger = pad.leftTrigger;
	r.bRightTrigger = pad.rightTrigger;
	r.sThumbLX = pad.lx;
	r.sThumbLY = pad.ly;
	r.sThumbRX = pad.rx;
	r.sThumbRY = pad.ry;
	vigem_target_x360_update((PVIGEM_CLIENT)m_client, (PVIGEM_TARGET)m_target, r);
}
