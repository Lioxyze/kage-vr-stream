// Minimal pose / matrix math for OpenXR + GLES (column-major matrices).
#pragma once

#include <openxr/openxr.h>

#include <cmath>

namespace xm {

struct Mat4 {
    float m[16];
};

inline XrQuaternionf mul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline XrQuaternionf conj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrQuaternionf p{v.x, v.y, v.z, 0};
    const XrQuaternionf r = mul(mul(q, p), conj(q));
    return {r.x, r.y, r.z};
}
inline XrVector3f add(const XrVector3f& a, const XrVector3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XrVector3f sub(const XrVector3f& a, const XrVector3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline XrVector3f scale(const XrVector3f& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(const XrVector3f& a, const XrVector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const XrVector3f& a) { return std::sqrt(dot(a, a)); }

// a ∘ b : b expressed in a's frame, returned in a's parent frame.
inline XrPosef compose(const XrPosef& a, const XrPosef& b) {
    return {mul(a.orientation, b.orientation), add(a.position, rotate(a.orientation, b.position))};
}
inline XrPosef inverse(const XrPosef& p) {
    const XrQuaternionf ci = conj(p.orientation);
    return {ci, scale(rotate(ci, p.position), -1.f)};
}

inline Mat4 fromPose(const XrPosef& p, float s = 1.f) {
    const auto& q = p.orientation;
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2, xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
    const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    return {{(1 - (yy + zz)) * s, (xy + wz) * s, (xz - wy) * s, 0, (xy - wz) * s, (1 - (xx + zz)) * s, (yz + wx) * s, 0,
             (xz + wy) * s, (yz - wx) * s, (1 - (xx + yy)) * s, 0, p.position.x, p.position.y, p.position.z, 1}};
}
inline Mat4 fromPoseScale(const XrPosef& p, float sx, float sy, float sz) {
    Mat4 m = fromPose(p);
    for (int i = 0; i < 3; ++i) {
        m.m[i] *= sx;
        m.m[4 + i] *= sy;
        m.m[8 + i] *= sz;
    }
    return m;
}
inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
inline Mat4 projection(const XrFovf& f, float n, float fa) {
    const float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
    const float w = r - l, h = u - d;
    return {{2 / w, 0, 0, 0, 0, 2 / h, 0, 0, (r + l) / w, (u + d) / h, -(fa + n) / (fa - n), -1, 0, 0, -2 * fa * n / (fa - n), 0}};
}
inline Mat4 viewProj(const XrView& v) { return mul(projection(v.fov, 0.05f, 100.f), fromPose(inverse(v.pose))); }

}  // namespace xm
