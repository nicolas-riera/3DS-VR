#pragma once
#include <math.h>

typedef struct { float w, x, y, z; } quat;
typedef struct { float x, y, z; } vec3;

static inline vec3 v3(float x, float y, float z) { return (vec3){x, y, z}; }
static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b) {
	return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3_len(vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline vec3 v3_norm(vec3 a) {
	float l = v3_len(a);
	return l > 1e-9f ? v3_scale(a, 1.0f / l) : v3(0, 0, 0);
}

static inline quat q_identity(void) { return (quat){1, 0, 0, 0}; }

static inline quat q_mul(quat a, quat b) {
	return (quat){
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	};
}

static inline quat q_conj(quat q) { return (quat){q.w, -q.x, -q.y, -q.z}; }

static inline quat q_norm(quat q) {
	float n = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	if (n < 1e-9f) return q_identity();
	n = 1.0f / n;
	return (quat){q.w * n, q.x * n, q.y * n, q.z * n};
}

static inline quat q_from_axis_angle(vec3 axis, float angle) {
	float s = sinf(angle * 0.5f);
	axis = v3_norm(axis);
	return (quat){cosf(angle * 0.5f), axis.x * s, axis.y * s, axis.z * s};
}

/* Rotation vector (axis * angle, rad) to quaternion, exact for any magnitude. */
static inline quat q_from_rotvec(vec3 r) {
	float a = v3_len(r);
	if (a < 1e-8f) return q_norm((quat){1, r.x * 0.5f, r.y * 0.5f, r.z * 0.5f});
	return q_from_axis_angle(v3_scale(r, 1.0f / a), a);
}

/* Rotates v by q (v' = q v q*). */
static inline vec3 q_rotate(quat q, vec3 v) {
	vec3 u = v3(q.x, q.y, q.z);
	vec3 t = v3_scale(v3_cross(u, v), 2.0f);
	return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}

/* Shortest rotation taking unit vector a onto unit vector b. */
static inline quat q_from_two_vectors(vec3 a, vec3 b) {
	float d = v3_dot(a, b);
	if (d < -0.999999f) {
		vec3 axis = v3_cross(v3(1, 0, 0), a);
		if (v3_len(axis) < 1e-6f) axis = v3_cross(v3(0, 1, 0), a);
		return q_from_axis_angle(axis, (float)M_PI);
	}
	vec3 c = v3_cross(a, b);
	return q_norm((quat){1.0f + d, c.x, c.y, c.z});
}
