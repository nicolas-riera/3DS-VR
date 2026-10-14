#include "render.h"

#include <citro2d.h>
#include <string.h>

#include "reproject_shbin.h"

typedef struct {
	float x, y, z;
	float u, v;
} Vertex;

static C3D_RenderTarget* s_top[2];
static C3D_RenderTarget* s_bottom;
static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_uMvp;
static C3D_AttrInfo s_attr;
static Vertex* s_vbo; /* 2 quads (left, right eye), triangle strips */
static bool s_3d;

bool render_init(void) {
	s_top[0] = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	s_top[1] = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	if (!s_top[0] || !s_top[1] || !s_bottom) return false;

	s_dvlb = DVLB_ParseFile((u32*)reproject_shbin, reproject_shbin_size);
	if (!s_dvlb) return false;
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uMvp = shaderInstanceGetUniformLocation(s_prog.vertexShader, "mvp");

	AttrInfo_Init(&s_attr);
	AttrInfo_AddLoader(&s_attr, 0, GPU_FLOAT, 3); /* v0: position */
	AttrInfo_AddLoader(&s_attr, 1, GPU_FLOAT, 2); /* v1: texcoord */

	s_vbo = (Vertex*)linearAlloc(sizeof(Vertex) * 8);
	return s_vbo != NULL;
}

void render_exit(void) {
	if (s_vbo) linearFree(s_vbo);
	s_vbo = NULL;
	if (s_dvlb) {
		shaderProgramFree(&s_prog);
		DVLB_Free(s_dvlb);
		s_dvlb = NULL;
	}
	gfxSet3D(false);
}

C3D_RenderTarget* render_target(gfx3dSide_t side) {
	return s_top[side == GFX_RIGHT ? 1 : 0];
}

C3D_RenderTarget* render_bottom_target(void) {
	return s_bottom;
}

void render_set_3d(bool on) {
	if (on == s_3d) return;
	s_3d = on;
	gfxSet3D(on);
}

bool render_3d(void) {
	return s_3d;
}

/* Rotation matrix of q (column vectors: v' = M v). */
static void mtx_from_quat(C3D_Mtx* m, quat q) {
	float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	Mtx_Zeros(m);
	m->r[0].x = 1 - 2 * (yy + zz);
	m->r[0].y = 2 * (xy - wz);
	m->r[0].z = 2 * (xz + wy);
	m->r[1].x = 2 * (xy + wz);
	m->r[1].y = 1 - 2 * (xx + zz);
	m->r[1].z = 2 * (yz - wx);
	m->r[2].x = 2 * (xz - wy);
	m->r[2].y = 2 * (yz + wx);
	m->r[2].z = 1 - 2 * (xx + yy);
	m->r[3].w = 1;
}

static void fill_quad(Vertex* q, float tx, float ty, float u1, float v0, float v1) {
	/* strip order: bottom-left, bottom-right, top-left, top-right */
	q[0] = (Vertex){-tx, -ty, -1, 0, v0};
	q[1] = (Vertex){tx, -ty, -1, u1, v0};
	q[2] = (Vertex){-tx, ty, -1, 0, v1};
	q[3] = (Vertex){tx, ty, -1, u1, v1};
}

static void bind_state(C3D_Tex* tex) {
	C3D_BindProgram(&s_prog);
	C3D_SetAttrInfo(&s_attr);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, s_vbo, sizeof(Vertex), 2, 0x10);

	C3D_TexBind(0, tex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));

	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
}

void render_video(const VideoView* v, quat current, float screenTanX, float screenTanY) {
	fill_quad(&s_vbo[0], v->encTanX, v->encTanY, v->u1, v->vLeft[0], v->vLeft[1]);
	fill_quad(&s_vbo[4], v->encTanX, v->encTanY, v->u1, v->vRight[0], v->vRight[1]);
	GSPGPU_FlushDataCache(s_vbo, sizeof(Vertex) * 8);

	/* frame space -> current head space */
	C3D_Mtx rot, proj, mvp;
	mtx_from_quat(&rot, q_norm(q_mul(q_conj(current), v->pose)));
	Mtx_PerspTilt(&proj, 2.0f * atanf(screenTanY), screenTanX / screenTanY, 0.05f, 100.0f, false);
	Mtx_Multiply(&mvp, &proj, &rot);

	int eyes = s_3d ? 2 : 1;
	for (int e = 0; e < eyes; e++) {
		C3D_RenderTarget* t = s_top[e];
		C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(t);
		bind_state(v->tex);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uMvp, &mvp);
		/* a mono stream shows the same picture to both eyes */
		int quad = (e == 1 && v->stereo) ? 1 : 0;
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, quad * 4, 4);
	}
}
