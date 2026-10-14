#include "gamepad.h"

#include <windows.h>

#include <ViGEm/Client.h>
#include <cstring>

#include "ipc.h"
#include "log.h"

uint8_t Gamepad::Connect() {
	if (m_target) return m_status;
	PVIGEM_CLIENT client = vigem_alloc();
	if (!client) return m_status = IPC_PAD_ERROR;
	VIGEM_ERROR err = vigem_connect(client);
	if (!VIGEM_SUCCESS(err)) {
		vigem_free(client);
		Log("ViGEmBus not available (0x%08X): no controller", (unsigned)err);
		return m_status = (err == VIGEM_ERROR_BUS_NOT_FOUND) ? IPC_PAD_NO_DRIVER : IPC_PAD_ERROR;
	}
	PVIGEM_TARGET target = vigem_target_x360_alloc();
	err = vigem_target_add(client, target);
	if (!VIGEM_SUCCESS(err)) {
		vigem_target_free(target);
		vigem_disconnect(client);
		vigem_free(client);
		Log("Could not plug the virtual Xbox 360 controller (0x%08X)", (unsigned)err);
		return m_status = IPC_PAD_ERROR;
	}
	m_client = client;
	m_target = target;
	m_hasLast = false;
	Log("Virtual Xbox 360 controller plugged in");
	return m_status = IPC_PAD_OK;
}

void Gamepad::Disconnect() {
	if (!m_target) return;
	PVIGEM_CLIENT client = (PVIGEM_CLIENT)m_client;
	PVIGEM_TARGET target = (PVIGEM_TARGET)m_target;
	vigem_target_remove(client, target);
	vigem_target_free(target);
	vigem_disconnect(client);
	vigem_free(client);
	m_client = m_target = nullptr;
	m_status = IPC_PAD_IDLE;
	Log("Virtual Xbox 360 controller unplugged");
}

void Gamepad::Update(const Vr3dsGamepad& pad) {
	if (!m_target) return;
	if (m_hasLast && memcmp(&pad, &m_last, sizeof(pad)) == 0) return;
	m_last = pad;
	m_hasLast = true;
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
