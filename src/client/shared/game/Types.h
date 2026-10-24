#pragma once
// Basic engine math types (layout-compatible with gta_sa.exe 1.0 US).
#include <cmath>
#include <cstdint>

namespace game {

struct CVector {
    float x = 0, y = 0, z = 0;
    CVector() = default;
    constexpr CVector(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    CVector operator+(const CVector& o) const { return {x + o.x, y + o.y, z + o.z}; }
    CVector operator-(const CVector& o) const { return {x - o.x, y - o.y, z - o.z}; }
    CVector operator*(float s) const { return {x * s, y * s, z * s}; }
    CVector& operator+=(const CVector& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float Length2D() const { return std::sqrt(x * x + y * y); }
    float Dot(const CVector& o) const { return x * o.x + y * o.y + z * o.z; }
    CVector Cross(const CVector& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    CVector Normalized() const {
        float l = Length();
        return l > 1e-6f ? CVector{x / l, y / l, z / l} : CVector{0, 0, 1};
    }
};
static_assert(sizeof(CVector) == 12);

struct CVector2D {
    float x = 0, y = 0;
};

struct RwV3d {
    float x, y, z;
};

struct RwRGBA {
    uint8_t r, g, b, a;
};

struct CRGBA {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

// RwMatrix-compatible matrix (CMatrix adds attachment fields after it).
struct CMatrix {
    CVector right;
    uint32_t flags;
    CVector forward;  // "up" in RW terms; SA calls it forward (Y axis)
    uint32_t pad1;
    CVector up;       // "at" in RW terms; Z axis
    uint32_t pad2;
    CVector pos;
    uint32_t pad3;
    void* attachMatrix;
    bool ownsAttachedMatrix;
    uint8_t pad4[3];
};
static_assert(sizeof(CMatrix) == 0x48);

struct CQuaternion {
    float x = 0, y = 0, z = 0, w = 1;
};

// Simple transform used by CPlaceable when no matrix is allocated.
struct CSimpleTransform {
    CVector pos;
    float heading;
};

inline float WrapAngle(float a) {
    // in one step: subtracting 2pi in a loop never ends for a large angle (1e9, the "not set yet" heading of a held
    // cutscene partner, is a multiple of the float step there and froze the game in the cutscene)
    if (!std::isfinite(a)) return 0.0f;
    return std::remainder(a, 6.2831853f);
}

inline float LerpAngle(float a, float b, float t) { return WrapAngle(a + WrapAngle(b - a) * t); }

}  // namespace game
