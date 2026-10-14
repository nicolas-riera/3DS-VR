#pragma once
#include <cmath>

#include <openvr_driver.h>

struct Quat {
	float w = 1, x = 0, y = 0, z = 0;
};

inline Quat QuatFromMatrix(const vr::HmdMatrix34_t& m) {
	const float(*a)[4] = m.m;
	Quat q;
	float trace = a[0][0] + a[1][1] + a[2][2];
	if (trace > 0) {
		float s = 0.5f / std::sqrt(trace + 1.0f);
		q.w = 0.25f / s;
		q.x = (a[2][1] - a[1][2]) * s;
		q.y = (a[0][2] - a[2][0]) * s;
		q.z = (a[1][0] - a[0][1]) * s;
	} else if (a[0][0] > a[1][1] && a[0][0] > a[2][2]) {
		float s = 2.0f * std::sqrt(1.0f + a[0][0] - a[1][1] - a[2][2]);
		q.w = (a[2][1] - a[1][2]) / s;
		q.x = 0.25f * s;
		q.y = (a[0][1] + a[1][0]) / s;
		q.z = (a[0][2] + a[2][0]) / s;
	} else if (a[1][1] > a[2][2]) {
		float s = 2.0f * std::sqrt(1.0f + a[1][1] - a[0][0] - a[2][2]);
		q.w = (a[0][2] - a[2][0]) / s;
		q.x = (a[0][1] + a[1][0]) / s;
		q.y = 0.25f * s;
		q.z = (a[1][2] + a[2][1]) / s;
	} else {
		float s = 2.0f * std::sqrt(1.0f + a[2][2] - a[0][0] - a[1][1]);
		q.w = (a[1][0] - a[0][1]) / s;
		q.x = (a[0][2] + a[2][0]) / s;
		q.y = (a[1][2] + a[2][1]) / s;
		q.z = 0.25f * s;
	}
	float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	if (n > 0) {
		q.w /= n;
		q.x /= n;
		q.y /= n;
		q.z /= n;
	}
	return q;
}

// v' = q v q*
inline void QuatRotate(const Quat& q, const float v[3], float out[3]) {
	float ux = q.x, uy = q.y, uz = q.z;
	float tx = 2 * (uy * v[2] - uz * v[1]);
	float ty = 2 * (uz * v[0] - ux * v[2]);
	float tz = 2 * (ux * v[1] - uy * v[0]);
	out[0] = v[0] + q.w * tx + (uy * tz - uz * ty);
	out[1] = v[1] + q.w * ty + (uz * tx - ux * tz);
	out[2] = v[2] + q.w * tz + (ux * ty - uy * tx);
}

inline vr::HmdQuaternion_t ToHmdQuat(const Quat& q) {
	return {q.w, q.x, q.y, q.z};
}
