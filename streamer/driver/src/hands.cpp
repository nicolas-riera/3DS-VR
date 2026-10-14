#include "hands.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "log.h"

using namespace vr;

// Where the hands are held, in head space (+X right, +Y up, -Z forward): low
// in the view, as if holding a pointer in each hand.
static constexpr float kHandSide = 0.12f, kHandDown = 0.14f, kHandForward = 0.32f;
// The Touch render model aims (its "tip" and OpenXR aim poses) about 38
// degrees below its raw pose: tilting the raw pose up by as much makes both
// hands point where the console looks.
static constexpr float kAimPitchDeg = 38.0f;

EVRInitError Hand::Activate(uint32_t unObjectId) {
	CVRPropertyHelpers* p = VRProperties();
	PropertyContainerHandle_t props = p->TrackedDeviceToPropertyContainer(unObjectId);
	const char* side = m_left ? "left" : "right";

	p->SetStringProperty(props, Prop_TrackingSystemName_String, "oculus");
	p->SetStringProperty(props, Prop_ManufacturerName_String, "Oculus");
	p->SetStringProperty(props, Prop_ModelNumber_String,
						 m_left ? "Oculus Quest2 (Left Controller)" : "Oculus Quest2 (Right Controller)");
	p->SetStringProperty(props, Prop_SerialNumber_String, Serial());
	p->SetStringProperty(props, Prop_ControllerType_String, "oculus_touch");
	p->SetStringProperty(props, Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
	p->SetStringProperty(props, Prop_RenderModelName_String,
						 (std::string("oculus_quest2_controller_") + side).c_str());
	p->SetInt32Property(props, Prop_ControllerRoleHint_Int32,
						m_left ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand);
	p->SetBoolProperty(props, Prop_WillDriftInYaw_Bool, false);
	p->SetBoolProperty(props, Prop_DeviceIsWireless_Bool, true);
	p->SetBoolProperty(props, Prop_DeviceProvidesBatteryStatus_Bool, false); // the headset shows it
	std::string icons = std::string("{oculus}/icons/cb_") + side + "_controller_";
	p->SetStringProperty(props, Prop_NamedIconPathDeviceOff_String, (icons + "off.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceSearching_String, (icons + "searching.gif").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceSearchingAlert_String, (icons + "searching_alert.gif").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceReady_String, (icons + "ready.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceReadyAlert_String, (icons + "ready_alert.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceNotReady_String, (icons + "error.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceStandby_String, (icons + "off.png").c_str());
	p->SetStringProperty(props, Prop_NamedIconPathDeviceAlertLow_String, (icons + "ready_low.png").c_str());

	IVRDriverInput* in = VRDriverInput();
	std::lock_guard<std::mutex> lock(m_mutex);
	std::string lower = m_left ? "/input/x" : "/input/a", upper = m_left ? "/input/y" : "/input/b";
	in->CreateBooleanComponent(props, (lower + "/click").c_str(), &m_buttons[BTN_LOWER]);
	in->CreateBooleanComponent(props, (lower + "/touch").c_str(), &m_touches[T_LOWER]);
	in->CreateBooleanComponent(props, (upper + "/click").c_str(), &m_buttons[BTN_UPPER]);
	in->CreateBooleanComponent(props, (upper + "/touch").c_str(), &m_touches[T_UPPER]);
	in->CreateBooleanComponent(props, "/input/system/click", &m_buttons[BTN_SYSTEM]);
	in->CreateBooleanComponent(props, "/input/system/touch", &m_touches[T_SYSTEM]);
	in->CreateBooleanComponent(props, "/input/joystick/click", &m_buttons[BTN_STICK]);
	in->CreateBooleanComponent(props, "/input/joystick/touch", &m_touches[T_STICK]);
	in->CreateBooleanComponent(props, "/input/trigger/touch", &m_touches[T_TRIGGER]);
	in->CreateBooleanComponent(props, "/input/grip/touch", &m_touches[T_GRIP]);
	in->CreateBooleanComponent(props, "/input/thumbrest/touch", &m_touches[T_THUMBREST]);
	in->CreateScalarComponent(props, "/input/joystick/x", &m_axes[AX_X], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedTwoSided);
	in->CreateScalarComponent(props, "/input/joystick/y", &m_axes[AX_Y], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedTwoSided);
	in->CreateScalarComponent(props, "/input/trigger/value", &m_axes[AX_TRIGGER], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedOneSided);
	in->CreateScalarComponent(props, "/input/grip/value", &m_axes[AX_GRIP], VRScalarType_Absolute,
							  VRScalarUnits_NormalizedOneSided);
	in->CreateHapticComponent(props, "/output/haptic", &m_haptic); // games expect it; the 3DS has no motor

	m_pose = {};
	m_pose.qWorldFromDriverRotation.w = 1;
	m_pose.qDriverFromHeadRotation.w = 1;
	m_pose.qRotation.w = 1;
	m_pose.result = TrackingResult_Uninitialized;
	m_id = unObjectId;
	Log("%s hand activated (device %u)", m_left ? "Left" : "Right", unObjectId);
	return VRInitError_None;
}

void Hand::Deactivate() {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_id = k_unTrackedDeviceIndexInvalid;
}

void Hand::DebugRequest(const char*, char* pchResponseBuffer, uint32_t unResponseBufferSize) {
	if (unResponseBufferSize >= 1) pchResponseBuffer[0] = 0;
}

DriverPose_t Hand::GetPose() {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_pose;
}

void Hand::Update(const Vr3dsGamepad& pad, bool connected, const Quat& head, const float headPos[3]) {
	std::lock_guard<std::mutex> lock(m_mutex);
	uint32_t id = m_id;
	if (id == k_unTrackedDeviceIndexInvalid) return;
	bool changed = connected != m_connected;
	m_connected = connected;
	if (!connected && !changed) return; // already released and disconnected

	// pose: attached to the view
	float offset[3] = {m_left ? -kHandSide : kHandSide, -kHandDown, -kHandForward}, r[3];
	QuatRotate(head, offset, r);
	float half = kAimPitchDeg * 3.14159265f / 360.0f;
	Quat q = QuatMul(head, Quat{std::cos(half), std::sin(half), 0, 0});
	for (int i = 0; i < 3; i++) m_pose.vecPosition[i] = headPos[i] + r[i];
	m_pose.qRotation = ToHmdQuat(q);
	m_pose.poseIsValid = connected;
	m_pose.deviceIsConnected = connected;
	m_pose.result = connected ? TrackingResult_Running_OK : TrackingResult_Running_OutOfRange;
	VRServerDriverHost()->TrackedDevicePoseUpdated(id, m_pose, sizeof(m_pose));

	// buttons: the left hand gets the left side of the console, the right hand the right side
	Vr3dsGamepad p = pad;
	if (!connected) memset(&p, 0, sizeof(p)); // release everything
	uint16_t b = p.buttons;
	bool lower = b & (m_left ? VR3DS_BTN_X : VR3DS_BTN_A);
	bool upper = b & (m_left ? VR3DS_BTN_Y : VR3DS_BTN_B);
	bool system = m_left && (b & VR3DS_BTN_START); // the right one is SteamVR's (dashboard)
	bool click = b & (m_left ? VR3DS_BTN_LTHUMB : VR3DS_BTN_RTHUMB);
	bool grip = b & (m_left ? VR3DS_BTN_LSHOULDER : VR3DS_BTN_RSHOULDER);
	float trigger = (m_left ? p.leftTrigger : p.rightTrigger) / 255.0f;
	float x = std::max(-1.0f, (m_left ? p.lx : p.rx) / 32767.0f);
	float y = std::max(-1.0f, (m_left ? p.ly : p.ry) / 32767.0f);
	if (m_left && x == 0 && y == 0) {
		// the D-Pad moves the left stick too (when the Circle Pad is centred)
		x = (b & VR3DS_BTN_DPAD_RIGHT) ? 1.0f : (b & VR3DS_BTN_DPAD_LEFT) ? -1.0f : 0.0f;
		y = (b & VR3DS_BTN_DPAD_UP) ? 1.0f : (b & VR3DS_BTN_DPAD_DOWN) ? -1.0f : 0.0f;
	}
	bool stickTouched = click || std::fabs(x) > 0.05f || std::fabs(y) > 0.05f;

	IVRDriverInput* in = VRDriverInput();
	in->UpdateBooleanComponent(m_buttons[BTN_LOWER], lower, 0);
	in->UpdateBooleanComponent(m_buttons[BTN_UPPER], upper, 0);
	in->UpdateBooleanComponent(m_buttons[BTN_SYSTEM], system, 0);
	in->UpdateBooleanComponent(m_buttons[BTN_STICK], click, 0);
	in->UpdateBooleanComponent(m_touches[T_LOWER], lower, 0);
	in->UpdateBooleanComponent(m_touches[T_UPPER], upper, 0);
	in->UpdateBooleanComponent(m_touches[T_SYSTEM], system, 0);
	in->UpdateBooleanComponent(m_touches[T_STICK], stickTouched, 0);
	in->UpdateBooleanComponent(m_touches[T_TRIGGER], trigger > 0.0f, 0);
	in->UpdateBooleanComponent(m_touches[T_GRIP], grip, 0);
	in->UpdateBooleanComponent(m_touches[T_THUMBREST], false, 0);
	in->UpdateScalarComponent(m_axes[AX_X], x, 0);
	in->UpdateScalarComponent(m_axes[AX_Y], y, 0);
	in->UpdateScalarComponent(m_axes[AX_TRIGGER], trigger, 0);
	in->UpdateScalarComponent(m_axes[AX_GRIP], grip ? 1.0f : 0.0f, 0);
}
