#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <cstdint>
#include <functional>

// Simple 3D vector (double precision to match other translation units)
struct Vec3 {
    double x, y, z;
    Vec3() = default;
    Vec3(double X, double Y, double Z) : x(X), y(Y), z(Z) {}
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator*(double s) const { return Vec3(x * s, y * s, z * s); }
};

// Helper: clamp integer between 0 and max-1
static inline int clamp_int(int v, int maxv) {
    if (v < 0) return 0;
    if (v >= maxv) return maxv - 1;
    return v;
}

// A convenient flat-hash for voxel triplet
struct VoxelKey {
    int x, y, z;
    bool operator==(const VoxelKey& o) const noexcept { return x == o.x && y == o.y && z == o.z; }
};
struct VoxelHash {
    size_t operator()(const VoxelKey& k) const noexcept {
        uint64_t a = static_cast<uint64_t>(static_cast<uint32_t>(k.x));
        uint64_t b = static_cast<uint64_t>(static_cast<uint32_t>(k.y));
        uint64_t c = static_cast<uint64_t>(static_cast<uint32_t>(k.z));
        return static_cast<size_t>((a * 73856093u) ^ (b * 19349663u) ^ (c * 83492791u));
    }
};

// ---------- triangle-box overlap test (Akenine-Möller) ----------
#define CROSS(dest, v1, v2) \
    dest.x = v1.y * v2.z - v1.z * v2.y; \
    dest.y = v1.z * v2.x - v1.x * v2.z; \
    dest.z = v1.x * v2.y - v1.y * v2.x;

static inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static inline void findMinMax(double x0, double x1, double x2, double& min, double& max) {
    min = std::min(x0, std::min(x1, x2));
    max = std::max(x0, std::max(x1, x2));
}

static inline bool planeBoxOverlap(const Vec3& normal, double d, const Vec3& maxbox) {
    double vminx = (normal.x > 0.0) ? -maxbox.x : maxbox.x;
    double vmaxx = (normal.x > 0.0) ? maxbox.x : -maxbox.x;
    double vminy = (normal.y > 0.0) ? -maxbox.y : maxbox.y;
    double vmaxy = (normal.y > 0.0) ? maxbox.y : -maxbox.y;
    double vminz = (normal.z > 0.0) ? -maxbox.z : maxbox.z;
    double vmaxz = (normal.z > 0.0) ? maxbox.z : -maxbox.z;
    double vmin = normal.x * vminx + normal.y * vminy + normal.z * vminz;
    double vmax = normal.x * vmaxx + normal.y * vmaxy + normal.z * vmaxz;
    if (vmin + d > 0.0) return false;
    if (vmax + d < 0.0) return false;
    return true;
}

bool triBoxOverlap(const Vec3& boxcenter, const Vec3& boxhalfsize,
                   Vec3 triv0, Vec3 triv1, Vec3 triv2) {
    // Move triangle so boxcenter is at the origin
    triv0 = triv0 - boxcenter;
    triv1 = triv1 - boxcenter;
    triv2 = triv2 - boxcenter;

    // Compute edges
    Vec3 e0 = triv1 - triv0;
    Vec3 e1 = triv2 - triv1;
    Vec3 e2 = triv0 - triv2;

    // 1) Test the 9 axes perpendicular to the triangle edges and the box faces
    double p0, p1, p2, minp, maxp, rad;
    Vec3 axis;

    // Helper lambda to test one axis
    auto axisTest = [&](const Vec3& a, double a_x, double a_y, double a_z) -> bool {
        p0 = triv0.x * a_x + triv0.y * a_y + triv0.z * a_z;
        p1 = triv1.x * a_x + triv1.y * a_y + triv1.z * a_z;
        p2 = triv2.x * a_x + triv2.y * a_y + triv2.z * a_z;
        findMinMax(p0, p1, p2, minp, maxp);
        rad = boxhalfsize.x * std::fabs(a_x) +
              boxhalfsize.y * std::fabs(a_y) +
              boxhalfsize.z * std::fabs(a_z);
        if (minp > rad || maxp < -rad) return false;
        return true;
    };

    // Edge 0
    axis.x = 0;          axis.y = -e0.z; axis.z = e0.y;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;
    axis.x = e0.z;       axis.y = 0;    axis.z = -e0.x;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;
    axis.x = -e0.y;      axis.y = e0.x; axis.z = 0;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;

    // Edge 1
    axis.x = 0;          axis.y = -e1.z; axis.z = e1.y;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;
    axis.x = e1.z;       axis.y = 0;    axis.z = -e1.x;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;
    axis.x = -e1.y;      axis.y = e1.x; axis.z = 0;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;

    // Edge 2
    axis.x = 0;          axis.y = -e2.z; axis.z = e2.y;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;
    axis.x = e2.z;       axis.y = 0;    axis.z = -e2.x;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;
    axis.x = -e2.y;      axis.y = e2.x; axis.z = 0;
    if (!axisTest(axis, axis.x, axis.y, axis.z)) return false;

    // 2) Test overlap in the {x,y,z} directions
    findMinMax(triv0.x, triv1.x, triv2.x, minp, maxp);
    if (minp > boxhalfsize.x || maxp < -boxhalfsize.x) return false;
    findMinMax(triv0.y, triv1.y, triv2.y, minp, maxp);
    if (minp > boxhalfsize.y || maxp < -boxhalfsize.y) return false;
    findMinMax(triv0.z, triv1.z, triv2.z, minp, maxp);
    if (minp > boxhalfsize.z || maxp < -boxhalfsize.z) return false;

    // 3) Test if the box intersects the plane of the triangle
    Vec3 normal;
    CROSS(normal, e0, e1);
    double d = -dot(normal, triv0);
    if (!planeBoxOverlap(normal, d, boxhalfsize)) return false;

    // Passed all tests -> overlap
    return true;
}
#undef CROSS

// ---------- voxelization function ----------
std::vector<std::array<int, 3>> voxelize_triangle(
    const Vec3& v0, const Vec3& v1, const Vec3& v2,
    const Vec3& gridOrigin, float voxelSize,
    const std::array<int, 3>& gridDims)
{
    // Compute world-space AABB of triangle
    float minx = std::min({v0.x, v1.x, v2.x});
    float miny = std::min({v0.y, v1.y, v2.y});
    float minz = std::min({v0.z, v1.z, v2.z});
    float maxx = std::max({v0.x, v1.x, v2.x});
    float maxy = std::max({v0.y, v1.y, v2.y});
    float maxz = std::max({v0.z, v1.z, v2.z});

    // Convert to voxel coordinates (indices)
    int imin = static_cast<int>(std::floor((minx - gridOrigin.x) / voxelSize));
    int jmin = static_cast<int>(std::floor((miny - gridOrigin.y) / voxelSize));
    int kmin = static_cast<int>(std::floor((minz - gridOrigin.z) / voxelSize));
    int imax = static_cast<int>(std::floor((maxx - gridOrigin.x) / voxelSize));
    int jmax = static_cast<int>(std::floor((maxy - gridOrigin.y) / voxelSize));
    int kmax = static_cast<int>(std::floor((maxz - gridOrigin.z) / voxelSize));

    // Clamp to grid bounds
    imin = clamp_int(imin, gridDims[0]);
    jmin = clamp_int(jmin, gridDims[1]);
    kmin = clamp_int(kmin, gridDims[2]);
    imax = clamp_int(imax, gridDims[0]);
    jmax = clamp_int(jmax, gridDims[1]);
    kmax = clamp_int(kmax, gridDims[2]);

    std::vector<std::array<int, 3>> out;
    out.reserve((imax - imin + 1) * (jmax - jmin + 1) * (kmax - kmin + 1) / 4 + 1);

    // Use a set to ensure uniqueness
    std::unordered_set<VoxelKey, VoxelHash> found;
    found.reserve(1024);

    Vec3 boxhalf(voxelSize * 0.5f, voxelSize * 0.5f, voxelSize * 0.5f);

    for (int k = kmin; k <= kmax; ++k) {
        for (int j = jmin; j <= jmax; ++j) {
            for (int i = imin; i <= imax; ++i) {
                // Compute voxel center in world coordinates
                Vec3 boxcenter(
                    gridOrigin.x + (i + 0.5f) * voxelSize,
                    gridOrigin.y + (j + 0.5f) * voxelSize,
                    gridOrigin.z + (k + 0.5f) * voxelSize
                );

                if (triBoxOverlap(boxcenter, boxhalf, v0, v1, v2)) {
                    VoxelKey key{i, j, k};
                    if (found.insert(key).second) {
                        out.push_back({i, j, k});
                    }
                }
            }
        }
    }

    return out;
}
