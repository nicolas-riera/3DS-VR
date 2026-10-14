#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "vr3ds_protocol.h"

/* What the touch screen adds to the physical buttons. */
typedef struct {
	bool l3, r3, guide;
	bool zl, zr;          /* Old 3DS family only */
	float rx, ry;         /* virtual right stick, -1..1 (Old 3DS family only) */
} TouchPad;

/*
 * Maps the console controls to an Xbox 360 controller:
 *   B/A/Y/X -> A/B/X/Y (Xbox positions), or A/B/X/Y (matching letters)
 *   L/R -> LB/RB, ZL/ZR -> LT/RT, Circle Pad -> left stick, C-Stick -> right stick,
 *   START/SELECT -> Start/Back, D-Pad -> D-Pad.
 * `held` are hidKeysHeld() bits; `masked` buttons are left out (shortcuts).
 */
void input_build(Vr3dsGamepad* out, u32 held, u32 masked, const TouchPad* touch, bool nintendoLayout);
