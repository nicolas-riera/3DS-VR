#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

#include <openvr_driver.h>

#include "math_util.h"
#include "vr3ds_protocol.h"

// "VR controllers" mode: two Quest 2 Touch controllers held in front of the
// view, turning with the head, with the console's buttons spread over them.
// Touch is used because every SteamVR game has bindings for it.
class Hand : public vr::ITrackedDeviceServerDriver {
public:
	explicit Hand(bool left) : m_left(left) {}
	const char* Serial() const { return m_left ? "3DSVR-HAND-LEFT" : "3DSVR-HAND-RIGHT"; }

	// From the network thread, for every TRACKING packet, with the headset's
	// pose; connected = false outside the VR controllers mode.
	void Update(const Vr3dsGamepad& pad, bool connected, const Quat& head, const float headPos[3]);

	vr::EVRInitError Activate(uint32_t unObjectId) override;
	void Deactivate() override;
	void EnterStandby() override {}
	void* GetComponent(const char*) override { return nullptr; }
	void DebugRequest(const char*, char* pchResponseBuffer, uint32_t unResponseBufferSize) override;
	vr::DriverPose_t GetPose() override;

private:
	enum Button { BTN_LOWER, BTN_UPPER, BTN_SYSTEM, BTN_STICK, BTN_COUNT }; // A/X, B/Y, menu, stick click
	enum Touch { T_LOWER, T_UPPER, T_SYSTEM, T_STICK, T_TRIGGER, T_GRIP, T_THUMBREST, T_COUNT };
	enum Axis { AX_X, AX_Y, AX_TRIGGER, AX_GRIP, AX_COUNT };

	const bool m_left;
	std::mutex m_mutex;
	std::atomic<uint32_t> m_id{vr::k_unTrackedDeviceIndexInvalid};
	vr::VRInputComponentHandle_t m_buttons[BTN_COUNT] = {};
	vr::VRInputComponentHandle_t m_touches[T_COUNT] = {};
	vr::VRInputComponentHandle_t m_axes[AX_COUNT] = {};
	vr::VRInputComponentHandle_t m_haptic = vr::k_ulInvalidInputComponentHandle;
	vr::DriverPose_t m_pose{};
	bool m_connected = false;
};
