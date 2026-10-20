#include "physx/havok_shapes.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace havok_shapes {
namespace {

constexpr int kMaxDepth = 6;
constexpr size_t kShapeBuffer = 1024;  // hkpShapeBuffer is 512 bytes; getChildShape may build the child in it

// hkpShape vtable slot 15: getContainer(). hkpShapeContainer vtable: 2 getFirstKey, 3 getNextKey, 5 getChildShape.
using GetContainer = const uint8_t*(__thiscall*)(const uint8_t* shape);
using FirstKey = uint32_t(__thiscall*)(const uint8_t* container);
using NextKey = uint32_t(__thiscall*)(const uint8_t* container, uint32_t key);
using ChildShape = const uint8_t*(__thiscall*)(const uint8_t* container, uint32_t key, void* buffer);
constexpr uint32_t kInvalidKey = 0xFFFFFFFFu;

template <typename Fn>
Fn vfn(const uint8_t* object, int slot) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void* const* const*>(object))[slot]);
}

const float* vec(const uint8_t* p, uint32_t off) { return reinterpret_cast<const float*>(p + off); }
const uint8_t* ptr(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const uint8_t* const*>(p + off); }

Transform translate(const float* t) {
    Transform r = Transform::identity();
    r.pos[0] = t[0];
    r.pos[1] = t[1];
    r.pos[2] = t[2];
    return r;
}

// hkTransformf: three hkVector4f columns, then the translation.
Transform from_hk_transform(const uint8_t* p) {
    Transform r;
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) r.rot[row][c] = vec(p, uint32_t(c * 16))[row];
    memcpy(r.pos, vec(p, 0x30), sizeof(r.pos));
    return r;
}

// hkQsTransformf: translation, rotation quaternion (x, y, z, w), scale.
Transform from_qs_transform(const uint8_t* p) {
    const float* q = vec(p, 0x10);
    const float* s = vec(p, 0x20);
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    Transform r;
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c) r.rot[row][c] = m[row][c] * s[c];
    memcpy(r.pos, vec(p, 0), sizeof(r.pos));
    return r;
}

#pragma warning(disable : 4324)  // Walker is padded for its 16-byte aligned shape buffers, as intended
struct Walker {
    alignas(16) uint8_t buffers[kMaxDepth + 1][kShapeBuffer];
    Sink& sink;
    bool ok = true;

    explicit Walker(Sink& s) : sink(s) {}

    void emit_convex(const float (*local)[3], int n, float radius, const Transform& t, uint32_t key) {
        float world[256][3];
        if (n > 256) n = 256;
        for (int i = 0; i < n; ++i) t.apply(local[i], world[i]);
        sink.convex(world, n, radius, key);
    }

    void ring(float (*out)[3], int& n, const float* a, const float* b, float r, int segments) {
        // Two rings of `segments` points around the segment a-b (cylinders and capsules, as hulls).
        float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-6f) d[0] = 0, d[1] = 1, d[2] = 0;
        else d[0] /= len, d[1] /= len, d[2] /= len;
        float u[3] = {std::fabs(d[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(d[0]) < 0.9f ? 0.0f : 1.0f, 0};
        // u = normalize(u - d * dot(u, d)), v = d x u
        const float k = u[0] * d[0] + u[1] * d[1] + u[2] * d[2];
        for (int i = 0; i < 3; ++i) u[i] -= d[i] * k;
        const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (float& c : u) c /= ul;
        const float v[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
        for (const float* end : {a, b})
            for (int s = 0; s < segments; ++s) {
                const float ang = 6.2831853f * float(s) / float(segments);
                const float cs = std::cos(ang) * r, sn = std::sin(ang) * r;
                for (int i = 0; i < 3; ++i) out[n][i] = end[i] + u[i] * cs + v[i] * sn;
                ++n;
            }
    }

    void leaf_or_container(const uint8_t* shape, const Transform& t, int depth, uint32_t key) {
        if (!shape) return;
        const uint8_t type = type_of(shape);
        switch (type) {
            case kTriangle: {
                float a[3], b[3], c[3];
                t.apply(vec(shape, 0x20), a);
                t.apply(vec(shape, 0x30), b);
                t.apply(vec(shape, 0x40), c);
                sink.triangle(a, b, c);
                return;
            }
            case kBox: {
                const float* h = vec(shape, 0x20);
                float v[8][3];
                for (int i = 0; i < 8; ++i) {
                    v[i][0] = (i & 1) ? h[0] : -h[0];
                    v[i][1] = (i & 2) ? h[1] : -h[1];
                    v[i][2] = (i & 4) ? h[2] : -h[2];
                }
                emit_convex(v, 8, *reinterpret_cast<const float*>(shape + 0x10), t, key);
                return;
            }
            case kConvexVertices: {
                // m_rotatedVertices: hkFourTransposedPointsf blocks (x[4], y[4], z[4]); m_numVertices.
                const uint8_t* blocks = ptr(shape, 0x40);
                const int n = *reinterpret_cast<const int*>(shape + 0x4C);
                if (!blocks || n <= 0) return;
                float v[256][3];
                const int count = n > 256 ? 256 : n;
                for (int i = 0; i < count; ++i) {
                    const float* b = reinterpret_cast<const float*>(blocks + (i / 4) * 48);
                    v[i][0] = b[i % 4];
                    v[i][1] = b[4 + i % 4];
                    v[i][2] = b[8 + i % 4];
                }
                emit_convex(v, count, *reinterpret_cast<const float*>(shape + 0x10), t, key);
                return;
            }
            case kCapsule:
            case kCylinder: {
                const float r = type == kCapsule ? *reinterpret_cast<const float*>(shape + 0x10)
                                                 : *reinterpret_cast<const float*>(shape + 0x14);
                float v[32][3];
                int n = 0;
                ring(v, n, vec(shape, 0x20), vec(shape, 0x30), r, 12);
                emit_convex(v, n, 0, t, key);
                return;
            }
            case kConvexTranslate:
                leaf_or_container(ptr(shape, 0x18), t * translate(vec(shape, 0x20)), depth, key);
                return;
            case kConvexTransform:
                leaf_or_container(ptr(shape, 0x18), t * from_qs_transform(shape + 0x20), depth, key);
                return;
            case kTransform:
                leaf_or_container(ptr(shape, 0x14), t * from_hk_transform(shape + 0x30), depth, key);
                return;
            case kMopp:
                leaf_or_container(ptr(shape, 0x34), t, depth, key);
                return;
            case kTriSampledHeightFieldBvTree:
                leaf_or_container(ptr(shape, 0x18), t, depth, key);
                return;
            case kTriSampledHeightFieldCollection:
                sink.height_field(ptr(shape, 0x18), t);
                return;
            case kSampledHeightField:
                sink.height_field(shape, t);
                return;
            default:
                break;
        }
        const uint8_t* container = vfn<GetContainer>(shape, 15)(shape);
        if (!container) {
            sink.unsupported(type);
            return;
        }
        if (depth >= kMaxDepth) {
            ok = false;
            return;
        }
        const auto first = vfn<FirstKey>(container, 2);
        const auto next = vfn<NextKey>(container, 3);
        const auto child = vfn<ChildShape>(container, 5);
        int guard = 0;
        for (uint32_t k = first(container); k != kInvalidKey && guard < 4000000; k = next(container, k), ++guard)
            leaf_or_container(child(container, k, buffers[depth + 1]), t, depth + 1, k);
    }
};

}  // namespace

Transform Transform::identity() {
    Transform t{};
    t.rot[0][0] = t.rot[1][1] = t.rot[2][2] = 1.0f;
    return t;
}

void Transform::apply(const float in[3], float out[3]) const {
    for (int r = 0; r < 3; ++r) out[r] = rot[r][0] * in[0] + rot[r][1] * in[1] + rot[r][2] * in[2] + pos[r];
}

Transform Transform::operator*(const Transform& l) const {
    Transform r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r.rot[i][j] = rot[i][0] * l.rot[0][j] + rot[i][1] * l.rot[1][j] + rot[i][2] * l.rot[2][j];
        r.pos[i] = rot[i][0] * l.pos[0] + rot[i][1] * l.pos[1] + rot[i][2] * l.pos[2] + pos[i];
    }
    return r;
}

uint8_t type_of(const uint8_t* shape) { return shape[8]; }

const char* type_name(uint8_t type) {
    static const char* const kNames[kTypeCount] = {
        "sphere", "cylinder", "triangle", "box", "capsule", "convex_vertices", "tri_sampled_hf_collection",
        "tri_sampled_hf_bvtree", "list", "mopp", "convex_translate", "convex_transform", "sampled_height_field",
        "extended_mesh", "transform", "compressed_mesh", "static_compound", "bv_compressed_mesh", "collection",
        "user0", "user1", "user2", "bv_tree", "convex", "convex_piece", "multi_sphere", "convex_list",
        "triangle_collection", "height_field", "sphere_rep", "bv", "plane", "phantom_callback", "multi_ray", "invalid"};
    return type < kTypeCount ? kNames[type] : "?";
}

bool walk(const uint8_t* shape, const Transform& t, Sink& sink) {
    Walker w(sink);  // 7 KB of shape buffers on the stack
    w.leaf_or_container(shape, t, 0, kInvalidKey);
    return w.ok;
}

const uint8_t* body_shape(const uint8_t* entity) { return ptr(entity, 0x10); }  // m_collidable.m_shape

Transform body_transform(const uint8_t* entity) {
    return from_hk_transform(entity + 0xE0 + 0x10);  // m_motion.m_motionState.m_transform
}

uint8_t body_motion_type(const uint8_t* entity) { return entity[0xE0 + 0x8]; }

}  // namespace havok_shapes
