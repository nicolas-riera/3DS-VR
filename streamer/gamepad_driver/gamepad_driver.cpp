// 3DS VR gamepad driver: a user-mode (UMDF 2) root device that XInput sees as
// a wired Xbox 360 controller.
//  - XInput (xinput1_3, xinput1_4, xinput9_1_0) enumerates the XUSB device
//    interface and talks to each device with the IOCTLs below. The layouts come
//    from public reverse engineering of the XInput DLLs (see the README).
//  - The SteamVR driver feeds the state through a private interface
//    (common/vr3ds_pad.h). The XUSB interface only exists while a feeder holds
//    the device open, so the controller is unplugged the rest of the time.
#include <windows.h>
#include <wdf.h>

#include "vr3ds_pad.h"

// {EC87F1E3-C13B-4100-B5F7-8B84D54260CB}
static const GUID GUID_DEVINTERFACE_XUSB = {0xec87f1e3, 0xc13b, 0x4100, {0xb5, 0xf7, 0x8b, 0x84, 0xd5, 0x42, 0x60, 0xcb}};

#define XUSB_IOCTL(fn, access) CTL_CODE(0x8000, fn, METHOD_BUFFERED, access)
#define IOCTL_XUSB_GET_INFORMATION    XUSB_IOCTL(0x800, FILE_READ_ACCESS)                     // 0x80006000
#define IOCTL_XUSB_GET_CAPABILITIES   XUSB_IOCTL(0x801, FILE_READ_ACCESS | FILE_WRITE_ACCESS) // 0x8000E004
#define IOCTL_XUSB_GET_LED_STATE      XUSB_IOCTL(0x802, FILE_READ_ACCESS | FILE_WRITE_ACCESS) // 0x8000E008
#define IOCTL_XUSB_GET_STATE          XUSB_IOCTL(0x803, FILE_READ_ACCESS | FILE_WRITE_ACCESS) // 0x8000E00C
#define IOCTL_XUSB_SET_STATE          XUSB_IOCTL(0x804, FILE_WRITE_ACCESS)                    // 0x8000A010
#define IOCTL_XUSB_WAIT_GUIDE_BUTTON  XUSB_IOCTL(0x805, FILE_WRITE_ACCESS)                    // 0x8000A014
#define IOCTL_XUSB_GET_BATTERY_INFO   XUSB_IOCTL(0x806, FILE_READ_ACCESS | FILE_WRITE_ACCESS) // 0x8000E018
#define IOCTL_XUSB_POWER_DOWN         XUSB_IOCTL(0x807, FILE_WRITE_ACCESS)                    // 0x8000A01C
#define IOCTL_XUSB_GET_AUDIO_INFO     XUSB_IOCTL(0x808, FILE_READ_ACCESS | FILE_WRITE_ACCESS) // 0x8000E020

// Protocol 1.2: state, LED, capabilities with VID/PID, battery, audio, power off.
// Callers send the version they speak in each request (1.1 or 1.2), and get
// the matching layout back. 1.0 requests (one byte) still get a 1.0 state.
#define XUSB_VERSION 0x0102
#define XUSB_VID 0x045E // Microsoft
#define XUSB_PID 0x028E // Xbox 360 Controller (wired)
#define XUSB_GUIDE 0x0400

#pragma pack(push, 1)

struct XusbInfo {
	USHORT version;
	UCHAR deviceCount; // controllers behind this interface
	UCHAR reserved[5]; // 0x80 in there marks a device XInput must skip
	USHORT vid, pid;
};

struct XusbRequest {
	USHORT version;
	UCHAR index;
};

struct XusbState0100 {
	UCHAR status; // 1 = connected
	UCHAR reserved0;
	UCHAR inputId;
	ULONG packet;
	UCHAR reserved1;
	Vr3dsPadReport pad;
};

struct XusbState0101 {
	USHORT version;
	UCHAR status;
	UCHAR reserved0;
	UCHAR inputId;
	ULONG packet;
	UCHAR reserved1[2];
	Vr3dsPadReport pad;
	UCHAR reserved2[5];
	UCHAR extraButtons;
};

struct XusbCaps0101 {
	USHORT version;
	UCHAR type, subType;
	Vr3dsPadReport pad; // resolution of each control
	UCHAR reserved[6];
	UCHAR largeMotor, smallMotor;
};

struct XusbCaps0102 {
	USHORT version;
	UCHAR type, subType;
	USHORT flags; // 1 force feedback, 2 wireless, 4 voice, 8 plug-in modules, 16 no navigation
	USHORT vid, pid, productVersion;
	ULONG reserved0;
	Vr3dsPadReport pad;
	UCHAR reserved1[6];
	UCHAR largeMotor, smallMotor;
};

struct XusbLedState {
	USHORT version;
	UCHAR led;
};

struct XusbSetState {
	UCHAR index;
	UCHAR led;
	UCHAR largeMotor, smallMotor;
	UCHAR flags; // 1 = LED, 2 = vibration
};

struct XusbBatteryRequest {
	USHORT version;
	UCHAR index;
	UCHAR deviceType; // 0 gamepad, 1 headset
};

struct XusbBattery {
	USHORT version;
	UCHAR type, level;
};

struct XusbAudioInfo {
	USHORT version;
	USHORT vid, pid;
	UCHAR inputId; // 0xFF: no headset
};

#pragma pack(pop)

static_assert(sizeof(XusbInfo) == 12 && sizeof(XusbState0100) == 20 && sizeof(XusbState0101) == 29 &&
				  sizeof(XusbCaps0101) == 24 && sizeof(XusbCaps0102) == 36,
			  "XUSB layouts");

struct PadContext {
	SRWLOCK lock;          // state below
	SRWLOCK plugLock;      // serializes plugging and unplugging
	WDFFILEOBJECT feeder;  // the SteamVR driver's handle, null when unplugged
	bool xusbCreated;
	Vr3dsPadReport pad;
	ULONG packet;
	Vr3dsPadFeedback feedback;
	WDFQUEUE guideWaits;   // pending IOCTL_XUSB_WAIT_GUIDE_BUTTON
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PadContext, GetPad)

extern "C" DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
static EVT_WDF_FILE_CLEANUP EvtFileCleanup;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

static void FillState(PadContext* ctx, XusbState0101* s) {
	RtlZeroMemory(s, sizeof(*s));
	s->version = XUSB_VERSION;
	s->inputId = 0xFF;
	AcquireSRWLockShared(&ctx->lock);
	if (ctx->feeder) {
		s->status = 1;
		s->packet = ctx->packet;
		s->pad = ctx->pad;
	}
	ReleaseSRWLockShared(&ctx->lock);
}

// Completes the pending guide button waits with the current state.
static void CompleteGuideWaits(PadContext* ctx) {
	WDFREQUEST req;
	while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->guideWaits, &req))) {
		XusbState0101* s;
		NTSTATUS st = WdfRequestRetrieveOutputBuffer(req, sizeof(*s), (PVOID*)&s, nullptr);
		if (NT_SUCCESS(st)) FillState(ctx, s);
		WdfRequestCompleteWithInformation(req, st, NT_SUCCESS(st) ? sizeof(*s) : 0);
	}
}

static NTSTATUS Plug(WDFDEVICE device, PadContext* ctx, WDFFILEOBJECT file) {
	NTSTATUS st = STATUS_SUCCESS;
	AcquireSRWLockExclusive(&ctx->plugLock);
	if (ctx->feeder) {
		st = ctx->feeder == file ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
	} else {
		// created after start, the interface stays disabled until enabled here
		if (!ctx->xusbCreated) {
			st = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_XUSB, nullptr);
			ctx->xusbCreated = NT_SUCCESS(st);
		}
		if (NT_SUCCESS(st)) {
			AcquireSRWLockExclusive(&ctx->lock);
			ctx->feeder = file;
			ReleaseSRWLockExclusive(&ctx->lock);
			WdfDeviceSetDeviceInterfaceState(device, &GUID_DEVINTERFACE_XUSB, nullptr, TRUE);
		}
	}
	ReleaseSRWLockExclusive(&ctx->plugLock);
	return st;
}

static void Unplug(WDFDEVICE device, PadContext* ctx, WDFFILEOBJECT file) {
	AcquireSRWLockExclusive(&ctx->plugLock);
	if (ctx->feeder && ctx->feeder == file) {
		AcquireSRWLockExclusive(&ctx->lock);
		ctx->feeder = nullptr;
		RtlZeroMemory(&ctx->pad, sizeof(ctx->pad));
		RtlZeroMemory(&ctx->feedback, sizeof(ctx->feedback));
		ctx->packet++;
		ReleaseSRWLockExclusive(&ctx->lock);
		WdfDeviceSetDeviceInterfaceState(device, &GUID_DEVINTERFACE_XUSB, nullptr, FALSE);
		CompleteGuideWaits(ctx); // "not connected"
	}
	ReleaseSRWLockExclusive(&ctx->plugLock);
}

static void EvtFileCleanup(WDFFILEOBJECT file) {
	WDFDEVICE device = WdfFileObjectGetDevice(file);
	Unplug(device, GetPad(device), file);
}

// Output buffer of at least `size` bytes, zeroed. Read the input first: with
// METHOD_BUFFERED both can share the same memory.
template <typename T> static T* Output(WDFREQUEST req) {
	T* out = nullptr;
	if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(req, sizeof(T), (PVOID*)&out, nullptr))) return nullptr;
	RtlZeroMemory(out, sizeof(T));
	return out;
}

template <typename T> static bool Input(WDFREQUEST req, T* in) {
	PVOID buf;
	if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(req, sizeof(T), &buf, nullptr))) return false;
	RtlCopyMemory(in, buf, sizeof(T));
	return true;
}

static void EvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST req, size_t outLen, size_t inLen, ULONG code) {
	WDFDEVICE device = WdfIoQueueGetDevice(queue);
	PadContext* ctx = GetPad(device);
	NTSTATUS st = STATUS_SUCCESS;
	size_t info = 0;

	// XUSB requests carry the index of the controller behind the interface: only 0 here
	XusbRequest xr = {};
	bool xusb = (code >> 16) == 0x8000;
	if (xusb && code != IOCTL_XUSB_GET_INFORMATION && code != IOCTL_XUSB_SET_STATE) {
		if (code == IOCTL_XUSB_GET_STATE && inLen < sizeof(xr)) {
			UCHAR index = 0; // protocol 1.0: the index alone
			if (!Input(req, &index)) st = STATUS_INVALID_PARAMETER;
			xr.index = index;
		} else if (!Input(req, &xr)) {
			st = STATUS_INVALID_PARAMETER;
		}
		if (NT_SUCCESS(st) && xr.index != 0) st = STATUS_INVALID_PARAMETER;
		if (!NT_SUCCESS(st)) {
			WdfRequestComplete(req, st);
			return;
		}
	}

	switch (code) {
	case IOCTL_XUSB_GET_INFORMATION:
		if (auto* o = Output<XusbInfo>(req)) {
			o->version = XUSB_VERSION;
			o->deviceCount = 1;
			o->vid = XUSB_VID;
			o->pid = XUSB_PID;
			info = sizeof(*o);
		} else st = STATUS_BUFFER_TOO_SMALL;
		break;

	case IOCTL_XUSB_GET_STATE:
		if (inLen < sizeof(xr)) {
			if (auto* o = Output<XusbState0100>(req)) {
				XusbState0101 s;
				FillState(ctx, &s);
				o->status = s.status;
				o->inputId = s.inputId;
				o->packet = s.packet;
				o->pad = s.pad;
				info = sizeof(*o);
			} else st = STATUS_BUFFER_TOO_SMALL;
		} else if (auto* o = Output<XusbState0101>(req)) {
			FillState(ctx, o);
			info = sizeof(*o);
		} else st = STATUS_BUFFER_TOO_SMALL;
		break;

	case IOCTL_XUSB_GET_CAPABILITIES: {
		const Vr3dsPadReport res = {0xF3FF, 0xFF, 0xFF, (SHORT)0xFFC0, (SHORT)0xFFC0, (SHORT)0xFFC0, (SHORT)0xFFC0};
		if (xr.version < 0x0102) {
			if (auto* o = Output<XusbCaps0101>(req)) {
				o->version = XUSB_VERSION;
				o->type = 1;    // XINPUT_DEVTYPE_GAMEPAD
				o->subType = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
				o->pad = res;
				o->largeMotor = o->smallMotor = 0xFF;
				info = sizeof(*o);
			} else st = STATUS_BUFFER_TOO_SMALL;
		} else if (auto* o = Output<XusbCaps0102>(req)) {
			o->version = XUSB_VERSION;
			o->type = 1;
			o->subType = 1;
			o->vid = XUSB_VID;
			o->pid = XUSB_PID;
			o->productVersion = 0x0114;
			o->pad = res;
			o->largeMotor = o->smallMotor = 0xFF;
			info = sizeof(*o);
		} else st = STATUS_BUFFER_TOO_SMALL;
		break;
	}

	case IOCTL_XUSB_GET_LED_STATE:
		if (auto* o = Output<XusbLedState>(req)) {
			o->version = XUSB_VERSION;
			AcquireSRWLockShared(&ctx->lock);
			o->led = ctx->feedback.led;
			ReleaseSRWLockShared(&ctx->lock);
			info = sizeof(*o);
		} else st = STATUS_BUFFER_TOO_SMALL;
		break;

	case IOCTL_XUSB_SET_STATE: {
		XusbSetState in;
		if (!Input(req, &in) || in.index != 0) {
			st = STATUS_INVALID_PARAMETER;
			break;
		}
		AcquireSRWLockExclusive(&ctx->lock);
		if (in.flags & 1) ctx->feedback.led = in.led;
		if (in.flags & 2) {
			ctx->feedback.largeMotor = in.largeMotor;
			ctx->feedback.smallMotor = in.smallMotor;
		}
		ReleaseSRWLockExclusive(&ctx->lock);
		break;
	}

	case IOCTL_XUSB_WAIT_GUIDE_BUTTON:
		if (outLen < sizeof(XusbState0101)) st = STATUS_BUFFER_TOO_SMALL;
		else {
			st = WdfRequestForwardToIoQueue(req, ctx->guideWaits);
			if (NT_SUCCESS(st)) return; // completed on the next Guide press
		}
		break;

	case IOCTL_XUSB_GET_BATTERY_INFO: {
		XusbBatteryRequest in;
		if (!Input(req, &in)) st = STATUS_INVALID_PARAMETER;
		else if (auto* o = Output<XusbBattery>(req)) {
			o->version = XUSB_VERSION;
			if (in.deviceType == 0) {
				o->type = 1;  // BATTERY_TYPE_WIRED
				o->level = 3; // BATTERY_LEVEL_FULL
			}
			info = sizeof(*o);
		} else st = STATUS_BUFFER_TOO_SMALL;
		break;
	}

	case IOCTL_XUSB_GET_AUDIO_INFO:
		if (auto* o = Output<XusbAudioInfo>(req)) {
			o->version = XUSB_VERSION;
			o->vid = XUSB_VID;
			o->pid = XUSB_PID;
			o->inputId = 0xFF;
			info = sizeof(*o);
		} else st = STATUS_BUFFER_TOO_SMALL;
		break;

	case IOCTL_XUSB_POWER_DOWN:
		break;

	case IOCTL_VR3DS_PAD_SET_STATE: {
		Vr3dsPadReport in;
		if (!Input(req, &in)) {
			st = STATUS_INVALID_PARAMETER;
			break;
		}
		WDFFILEOBJECT file = WdfRequestGetFileObject(req);
		AcquireSRWLockShared(&ctx->lock);
		bool plugged = ctx->feeder && ctx->feeder == file;
		ReleaseSRWLockShared(&ctx->lock);
		if (!plugged) st = Plug(device, ctx, file);
		if (!NT_SUCCESS(st)) break;
		AcquireSRWLockExclusive(&ctx->lock);
		bool guide = (in.buttons & XUSB_GUIDE) && !(ctx->pad.buttons & XUSB_GUIDE);
		if (memcmp(&in, &ctx->pad, sizeof(in)) != 0) {
			ctx->pad = in;
			ctx->packet++;
		}
		ReleaseSRWLockExclusive(&ctx->lock);
		if (guide) CompleteGuideWaits(ctx);
		break;
	}

	case IOCTL_VR3DS_PAD_GET_FEEDBACK:
		if (auto* o = Output<Vr3dsPadFeedback>(req)) {
			AcquireSRWLockShared(&ctx->lock);
			*o = ctx->feedback;
			ReleaseSRWLockShared(&ctx->lock);
			info = sizeof(*o);
		} else st = STATUS_BUFFER_TOO_SMALL;
		break;

	default:
		st = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}
	WdfRequestCompleteWithInformation(req, st, info);
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
	UNREFERENCED_PARAMETER(driver);
	WDF_FILEOBJECT_CONFIG fileConfig;
	WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, nullptr, nullptr, EvtFileCleanup);
	WdfDeviceInitSetFileObjectConfig(init, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PadContext);
	WDFDEVICE device;
	NTSTATUS st = WdfDeviceCreate(&init, &attributes, &device);
	if (!NT_SUCCESS(st)) return st;
	PadContext* ctx = GetPad(device);
	InitializeSRWLock(&ctx->lock);
	InitializeSRWLock(&ctx->plugLock);

	// a software device: the queues never wait for power
	WDF_IO_QUEUE_CONFIG queueConfig;
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
	queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
	queueConfig.PowerManaged = WdfFalse;
	st = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, nullptr);
	if (!NT_SUCCESS(st)) return st;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
	queueConfig.PowerManaged = WdfFalse;
	st = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->guideWaits);
	if (!NT_SUCCESS(st)) return st;

	// the XUSB interface is created when a feeder plugs the controller in
	return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_VR3DS_PAD, nullptr);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
	WDF_DRIVER_CONFIG config;
	WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
	return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
