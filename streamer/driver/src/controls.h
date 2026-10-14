#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

#include <openvr_driver.h>

#include "vr3ds_protocol.h"

// The console's buttons as a SteamVR gamepad (controller type "gamepad"),
// connected only while SteamVR's dashboard is open: SteamVR knows how to drive
// its dashboard with one (aim with the head, A or ZR to click, B to go back).
// Games use the virtual Xbox controller.
class Controls : public vr::ITrackedDeviceServerDriver {
public:
	// From the network thread, for every TRACKING packet; connected = false
	// when the session ends or the dashboard is closed.
	void Update(const Vr3dsGamepad& pad, bool connected);

	vr::EVRInitError Activate(uint32_t unObjectId) override;
	void Deactivate() override;
	void EnterStandby() override {}
	void* GetComponent(const char*) override { return nullptr; }
	void DebugRequest(const char*, char* pchResponseBuffer, uint32_t unResponseBufferSize) override;
	vr::DriverPose_t GetPose() override;

private:
	enum Button {
		BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, BTN_BACK, BTN_START, BTN_GUIDE,
		BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_L3, BTN_R3, BTN_COUNT
	};
	enum Axis { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT, AX_COUNT };

	vr::DriverPose_t Pose(bool connected) const;

	std::mutex m_mutex; // Update (network thread) vs. Activate / Deactivate
	std::atomic<uint32_t> m_id{vr::k_unTrackedDeviceIndexInvalid};
	vr::VRInputComponentHandle_t m_buttons[BTN_COUNT] = {};
	vr::VRInputComponentHandle_t m_axes[AX_COUNT] = {};
	bool m_connected = false;
};
