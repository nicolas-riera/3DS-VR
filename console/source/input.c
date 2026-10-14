#include "input.h"

#include <string.h>

#define CPAD_RANGE   150.0f /* Circle Pad reports about +-156 */
#define CSTICK_RANGE 140.0f /* C-Stick reports about +-146 */
#define DEADZONE     0.12f

static s16 axis(float v) {
	float a = v < 0 ? -v : v;
	if (a < DEADZONE) return 0;
	/* rescale so the stick still reaches full range after the dead zone */
	float s = (a - DEADZONE) / (1.0f - DEADZONE);
	if (s > 1.0f) s = 1.0f;
	float r = (v < 0 ? -s : s) * 32767.0f;
	return (s16)r;
}

void input_build(Vr3dsGamepad* out, u32 held, u32 masked, const TouchPad* touch, bool nintendoLayout) {
	memset(out, 0, sizeof(*out));
	u32 k = held & ~masked;
	u16 b = 0;

	if (nintendoLayout) {
		if (k & KEY_A) b |= VR3DS_BTN_A;
		if (k & KEY_B) b |= VR3DS_BTN_B;
		if (k & KEY_X) b |= VR3DS_BTN_X;
		if (k & KEY_Y) b |= VR3DS_BTN_Y;
	} else {
		/* same positions as on an Xbox pad: bottom = A, right = B, left = X, top = Y */
		if (k & KEY_B) b |= VR3DS_BTN_A;
		if (k & KEY_A) b |= VR3DS_BTN_B;
		if (k & KEY_Y) b |= VR3DS_BTN_X;
		if (k & KEY_X) b |= VR3DS_BTN_Y;
	}
	if (k & KEY_L) b |= VR3DS_BTN_LSHOULDER;
	if (k & KEY_R) b |= VR3DS_BTN_RSHOULDER;
	if (k & KEY_START) b |= VR3DS_BTN_START;
	if (k & KEY_SELECT) b |= VR3DS_BTN_BACK;
	if (k & KEY_DUP) b |= VR3DS_BTN_DPAD_UP;
	if (k & KEY_DDOWN) b |= VR3DS_BTN_DPAD_DOWN;
	if (k & KEY_DLEFT) b |= VR3DS_BTN_DPAD_LEFT;
	if (k & KEY_DRIGHT) b |= VR3DS_BTN_DPAD_RIGHT;
	if (touch->l3) b |= VR3DS_BTN_LTHUMB;
	if (touch->r3) b |= VR3DS_BTN_RTHUMB;
	if (touch->guide) b |= VR3DS_BTN_GUIDE;
	out->buttons = b;

	out->leftTrigger = ((k & KEY_ZL) || touch->zl) ? 255 : 0;
	out->rightTrigger = ((k & KEY_ZR) || touch->zr) ? 255 : 0;

	circlePosition cp;
	hidCircleRead(&cp);
	out->lx = axis(cp.dx / CPAD_RANGE);
	out->ly = axis(cp.dy / CPAD_RANGE);

	float rx = touch->rx, ry = touch->ry;
	circlePosition cs;
	hidCstickRead(&cs);
	if (cs.dx || cs.dy) {
		rx = cs.dx / CSTICK_RANGE;
		ry = cs.dy / CSTICK_RANGE;
	}
	out->rx = axis(rx);
	out->ry = axis(ry);
}
