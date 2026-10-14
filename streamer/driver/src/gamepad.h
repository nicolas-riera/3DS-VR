#pragma once
#include <cstdint>

#include "vr3ds_protocol.h"

// The console's controls as a virtual Xbox 360 controller (ViGEmBus).
class Gamepad {
public:
	~Gamepad() { Disconnect(); }
	// IpcGamepad value describing the result.
	uint8_t Connect();
	void Disconnect();
	void Update(const Vr3dsGamepad& pad);
	uint8_t Status() const { return m_status; }

private:
	void* m_client = nullptr; // PVIGEM_CLIENT
	void* m_target = nullptr; // PVIGEM_TARGET
	Vr3dsGamepad m_last{};
	bool m_hasLast = false;
	uint8_t m_status = 0;
};
