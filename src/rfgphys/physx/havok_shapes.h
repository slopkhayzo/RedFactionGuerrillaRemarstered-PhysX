#pragma once
#include <cstdint>

// Reads the game's Havok collision shapes (Havok 2014 layouts, research/docs/06 §5) into plain geometry for the
// PhysX host: triangles, convex hulls and height fields in world space. Containers (MOPP trees, extended,
// compressed and BV-compressed meshes, static compounds, lists, the game's rfg_destroyable_shape) are walked
// through Havok's own hkpShapeContainer interface, so no Havok data format is parsed by hand except the leaves.
// Simulation thread, while Havok is not stepping.
namespace havok_shapes {

struct Transform {  // world = rot * local + pos (rot: rows)
    float rot[3][3];
    float pos[3];
    static Transform identity();
    void apply(const float in[3], float out[3]) const;
    Transform operator*(const Transform& local) const;  // this after local
};

enum ShapeType : uint8_t {  // hkcdShapeType::ShapeTypeEnum
    kSphere = 0, kCylinder = 1, kTriangle = 2, kBox = 3, kCapsule = 4, kConvexVertices = 5,
    kTriSampledHeightFieldCollection = 6, kTriSampledHeightFieldBvTree = 7, kList = 8, kMopp = 9,
    kConvexTranslate = 10, kConvexTransform = 11, kSampledHeightField = 12, kExtendedMesh = 13, kTransform = 14,
    kCompressedMesh = 15, kStaticCompound = 16, kBvCompressedMesh = 17, kCollection = 18, kUser0 = 19,
    kUser1 = 20, kUser2 = 21, kBvTree = 22, kConvex = 23, kConvexPiece = 24, kMultiSphere = 25, kConvexList = 26,
    kTriangleCollection = 27, kHeightField = 28, kSphereRep = 29, kBv = 30, kPlane = 31, kPhantomCallback = 32,
    kMultiRay = 33, kTypeCount = 35
};

class Sink {
public:
    virtual ~Sink() = default;
    // World-space geometry. `key_path` identifies the leaf within its body (the shape key of the innermost
    // container), for pieces that can later disappear.
    virtual void triangle(const float a[3], const float b[3], const float c[3]) = 0;
    virtual void convex(const float (*vertices)[3], int count, float radius, uint32_t key) = 0;
    virtual void height_field(const uint8_t* shape, const Transform& t) = 0;
    virtual void unsupported(uint8_t type) = 0;
};

uint8_t type_of(const uint8_t* shape);
const char* type_name(uint8_t type);

// Walks `shape` (an hkpShape*) placed at `t`. Returns false if the walk hit something it could not read (the rest
// was still walked). Containers deeper than 6 levels are skipped.
bool walk(const uint8_t* shape, const Transform& t, Sink& sink);

// Body helpers (hkpEntity / hkpRigidBody).
const uint8_t* body_shape(const uint8_t* entity);
Transform body_transform(const uint8_t* entity);
uint8_t body_motion_type(const uint8_t* entity);  // hkpMotion::MotionType: 1 dynamic ... 5 fixed (keyframed 4)

}  // namespace havok_shapes
