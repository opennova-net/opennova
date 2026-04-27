#pragma once

#include <cstdint>
#include <cmath>

namespace opennova {

struct Vec3 {
    float x, y, z;

    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3 operator-(const Vec3& b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }

    float dot(const Vec3& b) const { return x * b.x + y * b.y + z * b.z; }

    Vec3 cross(const Vec3& b) const {
        return {b.z * y - z * b.y,
                z * b.x - b.z * x,
                b.y * x - b.x * y};
    }

    float length() const { return std::sqrt(x * x + y * y + z * z); }

    Vec3 normalized() const {
        float len = length();
        if (len == 0.0f) len = 0.1f;
        return {x / len, y / len, z / len};
    }
};

} // namespace opennova

