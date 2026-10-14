#include "controls.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "log.h"

using namespace vr;

static const char* const kButtonPaths[] = {
	"/input/a/click",        "/input/b/click",         "/input/x/click",          "/input/y/click",
	"/input/shoulder_left/click", "/input/shoulder_right/click", "/input/back/click", "/input/start/click",
	"/input/guide/click",    "/input/dpad_up/click",   "/input/dpad_down/click",  "/input/dpad_left/click",
	"/input/dpad_right/click", "/input/joystick_left/click", "/input/joystick_right/click"};
static const uint16_t kButtonBits[] = {
	VR3DS_BTN_A,         VR3DS_BTN_B,         VR3DS_BTN_X,         VR3DS_BTN_Y,          VR3DS_BTN_LSHOULDER,
	VR3DS_BTN_RSHOULDER, VR3DS_BTN_BACK,      VR3DS_BTN_START,     VR3DS_BTN_GUIDE,      VR3DS_BTN_DPAD_UP,
	VR3DS_BTN_DPAD_DOWN, VR3DS_BTN_DPAD_LEFT, VR3DS_BTN_DPAD_RIGHT, VR3DS_BTN_LTHUMB,    VR3DS_BTN_RTHUMB};

EVRInitError Controls::Activate(uint32_t unObjectId) {
	CVRPropertyHelpers* p = VRProperties();
	PropertyContainerHandle_t props = p->TrackedDeviceToPropertyContainer(unObjectId);

	p->SetStringProperty(props, Prop_TrackingSystemName_String, "3dsvr");
	p->SetStringProperty(props, Prop_SerialNumber_String, "3DSVR-CONTROLS");
	// also selects the status icons in driver.vrresources
	p->SetStringProperty(props, Prop_ModelNumber_String, "3DS VR Controls");
	p->SetStringProperty(props, Prop_ManufacturerName_String, "Nintendo");
	p->SetStringProperty(props, Prop_ControllerType_String, "gamepad");
	p->SetInt32Property(props, Prop_ControllerRoleHint_Int32, TrackedControllerRole_OptOut);
	p->SetStringProperty(props, Prop_InputProfilePath_String, "{3dsvr}/input/3dsvr_controls_profile.json");
	p->SetBoolProperty(props, Prop_NeverTracked_Bool, true);
	p->SetBoolProperty(props, Prop_WillDriftInYaw_Bool, false);
	p->SetBoolProperty(props, Prop_DeviceIsWireless_Bool, true);
	p->SetBoolProperty(props, Prop_DeviceProvidesBatteryStatus_Bool, false); // the headset shows it
	std::string icons = "{3dsvr}/icons/controls_status_";
	p->SetStringProperty(props, Prop_NamedIconPathDeviceOff_String, (icons + "off.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceSearching_String, (icons + "searching.gif").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceSearchingAlert_String, (icons + "searching_alert.gif").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceReady_String, (icons + "ready.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceReadyAlert_String, (icons + "ready_alert.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceNotReady_String, (icons + "error.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceStandby_String, (icons + "standby.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceStandbyAlert_String, (icons + "standby_alert.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceAlertLow_String, (icons + "ready_low.png").c_str());

	IVRDriverInput* in = VRDriverInput();
	std::lock_guard<std::mutex> lock(m_mutex);
	for (int i = 0; i < BTN_COUNT; i++) in->CreateBooleanComponent(props, kButtonPaths[i], &m_buttons[i]);
	in->CreateScalarComponent(props, "/input/joystick_left/x", &m_axes[AX_LX], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedTwoSided);
	in->CreateScalarComponent(props, "/input/joystick_left/y", &m_axes[AX_LY], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedTwoSided);
	in->CreateScalarComponent(props, "/input/joystick_right/x", &m_axes[AX_RX], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedTwoSided);
	in->CreateScalarComponent(props, "/input/joystick_right/y", &m_axes[AX_RY], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedTwoSided);
	in->CreateScalarComponent(props, "/input/trigger_left/value", &m_axes[AX_LT], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedOneSided);
	in->CreateScalarComponent(props, "/input/trigger_right/value", &m_axes[AX_RT], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedOneSided);
	m_id = unObjectId;
	Log("Console controls activated (device %u)", unObjectId);
	return VRInitError_None;
}

void Controls::Deactivate() {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_id = k_unTrackedDeviceIndexInvalid;
}

void Controls::DebugRequest(const char*, char* pchResponseBuffer, uint32_t unResponseBufferSize) {
	if (unResponseBufferSize >= 1) pchResponseBuffer[0] = 0;
}

DriverPose_t Controls::Pose(bool connected) const {
	// no pose of its own: SteamVR aims the dashboard pointer with the head
	DriverPose_t pose = {};
	pose.qWorldFromDriverRotation.w = 1;
	pose.qDriverFromHeadRotation.w = 1;
	pose.qRotation.w = 1;
	pose.result = TrackingResult_Uninitialized;
	pose.poseIsValid = false;
	pose.deviceIsConnected = connected;
	return pose;
}

DriverPose_t Controls::GetPose() {
	std::lock_guard<std::mutex> lock(m_mutex);
	return Pose(m_connected);
}

void Controls::Update(const Vr3dsGamepad& pad, bool connected) {
	std::lock_guard<std::mutex> lock(m_mutex);
	uint32_t id = m_id;
	if (id == k_unTrackedDeviceIndexInvalid) return;
	if (connected != m_connected) {
		m_connected = connected;
		VRServerDriverHost()->TrackedDevicePoseUpdated(id, Pose(connected), sizeof(DriverPose_t));
	}
	Vr3dsGamepad p = pad;
	if (!connected) memset(&p, 0, sizeof(p)); // release everything
	IVRDriverInput* in = VRDriverInput();
	for (int i = 0; i < BTN_COUNT; i++) in->UpdateBooleanComponent(m_buttons[i], (p.buttons & kButtonBits[i]) != 0, 0);
	auto axis = [](int16_t v) { return std::max(-1.0f, v / 32767.0f); };
	in->UpdateScalarComponent(m_axes[AX_LX], axis(p.lx), 0);
	in->UpdateScalarComponent(m_axes[AX_LY], axis(p.ly), 0);
	in->UpdateScalarComponent(m_axes[AX_RX], axis(p.rx), 0);
	in->UpdateScalarComponent(m_axes[AX_RY], axis(p.ry), 0);
	in->UpdateScalarComponent(m_axes[AX_LT], p.leftTrigger / 255.0f, 0);
	in->UpdateScalarComponent(m_axes[AX_RT], p.rightTrigger / 255.0f, 0);
}
