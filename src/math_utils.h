#pragma once

#include <cmath>
#include <cstring>

struct Vec3
{
    float x, y, z;

    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& v) const { return Vec3(x + v.x, y + v.y, z + v.z); }
    Vec3 operator-(const Vec3& v) const { return Vec3(x - v.x, y - v.y, z - v.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3& operator+=(const Vec3& v) { x += v.x; y += v.y; z += v.z; return *this; }

    float length() const { return sqrtf(x * x + y * y + z * z); }
    Vec3 normalized() const { float l = length(); return l > 0 ? Vec3(x / l, y / l, z / l) : Vec3(); }
};

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return Vec3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

inline float dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct Mat4
{
    float m[16]; // Column-major

    Mat4() { memset(m, 0, sizeof(m)); }

    static Mat4 identity()
    {
        Mat4 result;
        result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
        return result;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up)
    {
        Vec3 f = (target - eye).normalized();
        Vec3 r = cross(f, up).normalized();
        Vec3 u = cross(r, f);

        Mat4 result = identity();
        result.m[0] = r.x;  result.m[4] = r.y;  result.m[8]  = r.z;  result.m[12] = -dot(r, eye);
        result.m[1] = u.x;  result.m[5] = u.y;  result.m[9]  = u.z;  result.m[13] = -dot(u, eye);
        result.m[2] = -f.x; result.m[6] = -f.y; result.m[10] = -f.z; result.m[14] = dot(f, eye);
        result.m[3] = 0;    result.m[7] = 0;    result.m[11] = 0;    result.m[15] = 1;
        return result;
    }

    static Mat4 perspective(float fovY, float aspect, float nearZ, float farZ)
    {
        float tanHalfFov = tanf(fovY * 0.5f);

        Mat4 result;
        result.m[0] = 1.0f / (aspect * tanHalfFov);
        result.m[5] = 1.0f / tanHalfFov;
        result.m[10] = farZ / (nearZ - farZ);
        result.m[11] = -1.0f;
        result.m[14] = (nearZ * farZ) / (nearZ - farZ);
        return result;
    }

    static Mat4 orthographic(float left, float right, float bottom, float top, float nearZ, float farZ)
    {
        Mat4 result;
        result.m[0] = 2.0f / (right - left);
        result.m[5] = 2.0f / (top - bottom);
        result.m[10] = 1.0f / (nearZ - farZ);
        result.m[12] = (left + right) / (left - right);
        result.m[13] = (top + bottom) / (bottom - top);
        result.m[14] = nearZ / (nearZ - farZ);
        result.m[15] = 1.0f;
        return result;
    }

    Mat4 operator*(const Mat4& other) const
    {
        Mat4 result;
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                result.m[c * 4 + r] =
                    m[0 * 4 + r] * other.m[c * 4 + 0] +
                    m[1 * 4 + r] * other.m[c * 4 + 1] +
                    m[2 * 4 + r] * other.m[c * 4 + 2] +
                    m[3 * 4 + r] * other.m[c * 4 + 3];
            }
        }
        return result;
    }
};

struct Camera
{
    Vec3 position;
    float yaw;   // Horizontal rotation (radians)
    float pitch; // Vertical rotation (radians)
    float fov;
    float nearZ;
    float farZ;
    float moveSpeed;
    float lookSpeed;

    Camera()
        : position(0, 50, 100)
        , yaw(0)
        , pitch(-0.3f)
        , fov(1.0472f) // 60 degrees
        , nearZ(0.1f)
        , farZ(5000.0f)
        , moveSpeed(100.0f)
        , lookSpeed(0.002f)
    {}

    Vec3 getForward() const
    {
        return Vec3(
            sinf(yaw) * cosf(pitch),
            sinf(pitch),
            -cosf(yaw) * cosf(pitch)
        );
    }

    Vec3 getRight() const
    {
        return Vec3(cosf(yaw), 0, sinf(yaw));
    }

    Vec3 getUp() const
    {
        return Vec3(0, 1, 0);
    }

    Mat4 getViewMatrix() const
    {
        Vec3 forward = getForward();
        Vec3 target = position + forward;
        return Mat4::lookAt(position, target, getUp());
    }

    Mat4 getProjectionMatrix(float aspect) const
    {
        return Mat4::perspective(fov, aspect, nearZ, farZ);
    }

    Mat4 getViewProjectionMatrix(float aspect) const
    {
        return getProjectionMatrix(aspect) * getViewMatrix();
    }
};
