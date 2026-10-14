#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#include "mathx.h"
#include "video.h"

/*
 * Top screen: draws the last decoded frame with rotational reprojection.
 * Each eye's picture is a quad on the plane z = -1 of the pose the PC
 * rendered it with; drawing it with the console's current orientation
 * hides the network and decoding latency for head rotations.
 */

bool render_init(void);
void render_exit(void);

C3D_RenderTarget* render_target(gfx3dSide_t side);
C3D_RenderTarget* render_bottom_target(void);

/* Turns the stereoscopic top screen on or off (no effect on 2DS models). */
void render_set_3d(bool on);
bool render_3d(void);

/* Inside a C3D frame. Leaves citro2d unprepared: call C2D_Prepare() before 2D drawing. */
void render_video(const VideoView* v, quat current, float screenTanX, float screenTanY);
