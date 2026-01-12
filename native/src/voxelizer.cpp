#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_ENABLE_DRACO         // <-- enable Draco
#define TINYGLTF_ENABLE_MESHOPT       // <-- strongly recommended, Google tiles use this too
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "voxelizer.h"
#include <tiny_gltf.h>
#include <iostream>
#include <cmath>
#include <algorithm>

// --- Helper Math ---

struct Vec3 { double x, y, z; };
struct Vec2 { double u, v; };

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 operator*(const Vec3& a, double s) { return {a.x*s, a.y*s, a.z*s}; }
Vec3 operator/(const Vec3 &a, double s)
{
	return {a.x / s, a.y / s, a.z / s};
}

std::ostream &operator<<(std::ostream &s, const Vec3 &p)
{
	s << "(" << p.x << "," << p.y << "," << p.z << ")";
	return s;
}

// --- Voxelizer Implementation ---

#include "debug_log.h"

// ...

VoxelGrid Voxelizer::voxelize(const TileData &tile, int resolution, double originX,
		double originY, double originZ)
{
	VoxelGrid grid;
	tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

	const auto &glbData = tile.data;
    log_debug("[Voxelizer] Loading GLB (" + std::to_string(glbData.size()) + " bytes)");
	bool ret = loader.LoadBinaryFromMemory(&model, &err, &warn,

    if (!warn.empty()) log_debug("[Voxelizer] TinyGLTF Warn: " + warn);
    if (!err.empty()) log_debug("[Voxelizer] TinyGLTF Err: " + err);
    if (!ret) {
        log_debug("[Voxelizer] Failed to load GLB");
        return grid;
    }
    log_debug("[Voxelizer] GLB loaded. Meshes: " + std::to_string(model.meshes.size()));

    // 1. Extract mesh data (vertices, indices, UVs, materials)
    // Simplified: Iterate all meshes, apply world transform (if any), collect triangles.
    
    struct Triangle {
        Vec3 v0, v1, v2;
        Vec2 uv0, uv1, uv2;
        int materialIdx;
    };
    std::vector<Triangle> triangles;

    // Helper to get buffer data
    auto getBuffer = [&](int accessorIdx) -> const unsigned char* {
        if (accessorIdx < 0) return nullptr;
        const auto& accessor = model.accessors[accessorIdx];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];
        return buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
    };

    // Iterate nodes to find meshes
    // TODO: Handle hierarchy/transforms properly. For now, assume flat or simple.
    // Google 3D tiles usually have one mesh per node or simple hierarchy.
    
    for (const auto& node : model.nodes) {
        if (node.mesh < 0) continue;
        const auto& mesh = model.meshes[node.mesh];

        // Node transform
        // TODO: Apply node matrix/translation/rotation/scale
        
        for (const auto& primitive : mesh.primitives) {
            const float* posBuffer = nullptr;
            const float* uvBuffer = nullptr;
            const unsigned char* indicesBuffer = nullptr; // Could be u16 or u32
            int posStride = 0, uvStride = 0, indexType = 0;
            size_t vertexCount = 0, indexCount = 0;

            // Position
            if (primitive.attributes.count("POSITION")) {
                int accIdx = primitive.attributes.at("POSITION");
                const auto& acc = model.accessors[accIdx];
                posBuffer = (const float*)getBuffer(accIdx);
                posStride = acc.ByteStride(model.bufferViews[acc.bufferView]) ? acc.ByteStride(model.bufferViews[acc.bufferView]) / 4 : 3;
                vertexCount = acc.count;
            }

            // UV
            if (primitive.attributes.count("TEXCOORD_0")) {
                int accIdx = primitive.attributes.at("TEXCOORD_0");
                const auto& acc = model.accessors[accIdx];
                uvBuffer = (const float*)getBuffer(accIdx);
                uvStride = acc.ByteStride(model.bufferViews[acc.bufferView]) ? acc.ByteStride(model.bufferViews[acc.bufferView]) / 4 : 2;
            }

            // Indices
            if (primitive.indices >= 0) {
                const auto& acc = model.accessors[primitive.indices];
                indicesBuffer = getBuffer(primitive.indices);
                indexType = acc.componentType;
                indexCount = acc.count;
            }

            if (!posBuffer) continue;

            auto getPos = [&](int idx) -> Vec3 {
                return { (double)posBuffer[idx * posStride], (double)posBuffer[idx * posStride + 1], (double)posBuffer[idx * posStride + 2] };
            };
            auto getUV = [&](int idx) -> Vec2 {
                if (!uvBuffer) return {0,0};
                return { (double)uvBuffer[idx * uvStride], (double)uvBuffer[idx * uvStride + 1] };
            };

            if (indicesBuffer) {
                for (size_t i = 0; i < indexCount; i += 3) {
                    int i0, i1, i2;
                    if (indexType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        i0 = ((unsigned short*)indicesBuffer)[i];
                        i1 = ((unsigned short*)indicesBuffer)[i+1];
                        i2 = ((unsigned short*)indicesBuffer)[i+2];
                    } else if (indexType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        i0 = ((unsigned int*)indicesBuffer)[i];
                        i1 = ((unsigned int*)indicesBuffer)[i+1];
                        i2 = ((unsigned int*)indicesBuffer)[i+2];
                    } else {
                        i0 = ((unsigned char*)indicesBuffer)[i];
                        i1 = ((unsigned char*)indicesBuffer)[i+1];
                        i2 = ((unsigned char*)indicesBuffer)[i+2];
                    }
                    triangles.push_back({getPos(i0), getPos(i1), getPos(i2), getUV(i0), getUV(i1), getUV(i2), primitive.material});
                }
            } else {
                for (size_t i = 0; i < vertexCount; i += 3) {
                    triangles.push_back({getPos(i), getPos(i+1), getPos(i+2), getUV(i), getUV(i+1), getUV(i+2), primitive.material});
                }
            }
        }
    }

	Vec3 box_center{0, 0, 0};
	double box_size = 0;
	if (!tile.box.empty()) {
		box_center = {tile.box[0], tile.box[1], tile.box[2]};
		box_size = tile.box[11] * 2; // max?
    }

    // 2. Use Provided Origin (Global Origin)
    Vec3 center = { originX, originY, originZ };

    // Compute Rotation to align Up (center) to Y (0,1,0)
    // Up vector = normalize(center)
    double len = std::sqrt(center.x*center.x + center.y*center.y + center.z*center.z);
    Vec3 up = {center.x/len, center.y/len, center.z/len};
    Vec3 targetUp = {0, 1, 0};

    // Rotation quaternion from up to targetUp
    // Axis = cross(up, targetUp)
    Vec3 axis = {
        up.y*targetUp.z - up.z*targetUp.y,
        up.z*targetUp.x - up.x*targetUp.z,
        up.x*targetUp.y - up.y*targetUp.x
    };
    double dot = up.x*targetUp.x + up.y*targetUp.y + up.z*targetUp.z;
    
    // Construct rotation matrix (simplified for vector rotation)
    // v' = v * cos(theta) + cross(k, v) * sin(theta) + k * dot(k, v) * (1 - cos(theta))
    // But we can just build a basis.
    // Let's use a simple lookAt-style matrix or quaternion conversion.
    
    // Quat q = FromTwoVectors(up, targetUp)
    double s = std::sqrt((1+dot)*2);
    double invs = 1.0 / s;
    double qx = axis.x * invs;
    double qy = axis.y * invs;
    double qz = axis.z * invs;
    double qw = s * 0.5;

    auto rotate = [&](Vec3 v, Vec3 center = {0,0,0}) -> Vec3 {
        // v - center
        double vx = v.x - center.x;
        double vy = v.y - center.y;
        double vz = v.z - center.z;

        // Apply quaternion
        double ix = qw*vx + qy*vz - qz*vy;
        double iy = qw*vy + qz*vx - qx*vz;
        double iz = qw*vz + qx*vy - qy*vx;
        double iw = -qx*vx - qy*vy - qz*vz;

        return {
            ix*qw + iw*-qx + iy*-qz - iz*-qy,
            iy*qw + iw*-qy + iz*-qx - ix*-qz,
            iz*qw + iw*-qz + ix*-qy - iy*-qx
        };
    };

	Vec3 rotated_tile_box_center{0, 0, 0};
	if (!tile.box.empty())
	{
		const Vec3 tile_box_center = {tile.box[0], tile.box[1], tile.box[2]};
		rotated_tile_box_center = rotate(tile_box_center, center);
	}
	const auto rotated_half_center = rotated_tile_box_center / 2;
    // Transform all triangles
    for (auto& t : triangles) {
        t.v0 = rotate(t.v0);
        t.v1 = rotate(t.v1);
        t.v2 = rotate(t.v2);
        t.v0 = t.v0 + rotated_tile_box_center;
		t.v1 = t.v1 + rotated_tile_box_center;
		t.v2 = t.v2 + rotated_tile_box_center;
    }

    // Recompute BBox after rotation
    Vec3 min = {1e9, 1e9, 1e9}, max = {-1e9, -1e9, -1e9};
    for (const auto& t : triangles) {
        for (const auto& v : {t.v0, t.v1, t.v2}) {
            if (v.x < min.x) min.x = v.x; if (v.x > max.x) max.x = v.x;
            if (v.y < min.y) min.y = v.y; if (v.y > max.y) max.y = v.y;
            if (v.z < min.z) min.z = v.z; if (v.z > max.z) max.z = v.z;
        }
    }

    // 3. Rasterize (Simple 3D point-in-tri or conservative rasterization)
    // For simplicity, let's do a basic grid traversal or point sampling.
    // Given the resolution (e.g. 200), we define voxel size.
    
    double maxDim = std::max({max.x - min.x, max.y - min.y, max.z - min.z});
    double voxelSize = maxDim / resolution;
    if (voxelSize <= 0) return grid;

    int nx = std::ceil((max.x - min.x) / voxelSize);
    int ny = std::ceil((max.y - min.y) / voxelSize);
    int nz = std::ceil((max.z - min.z) / voxelSize);

    // Sparse grid map? Or dense if small enough.
    // Let's use a simple vector of voxels.
    
    // Very basic rasterizer: Check center of each voxel? Too slow (N^3).
    // Triangle-box intersection is better.
    // For this prototype, let's iterate triangles and find intersecting voxels.
    
    for (const auto& tri : triangles) {
        // BBox of triangle
        double tMinX = std::min({tri.v0.x, tri.v1.x, tri.v2.x});
        double tMaxX = std::max({tri.v0.x, tri.v1.x, tri.v2.x});
        double tMinY = std::min({tri.v0.y, tri.v1.y, tri.v2.y});
        double tMaxY = std::max({tri.v0.y, tri.v1.y, tri.v2.y});
        double tMinZ = std::min({tri.v0.z, tri.v1.z, tri.v2.z});
        double tMaxZ = std::max({tri.v0.z, tri.v1.z, tri.v2.z});

        int minX = std::max(0, (int)((tMinX - min.x) / voxelSize));
        int maxX = std::min(nx-1, (int)((tMaxX - min.x) / voxelSize));
        int minY = std::max(0, (int)((tMinY - min.y) / voxelSize));
        int maxY = std::min(ny-1, (int)((tMaxY - min.y) / voxelSize));
        int minZ = std::max(0, (int)((tMinZ - min.z) / voxelSize));

				int minX = tMinX;
				int maxX = tMaxX;
				int minY = tMinY;
				int maxY = tMaxY;
				int minZ = tMinZ;
				int maxZ = tMaxZ;
				int voxelSizeInt = std::max(1, int(voxelSize));
				for (int z = minZ; z <= maxZ; z+=voxelSizeInt) {
					for (int y = minY; y <= maxY; y+=voxelSizeInt) {
						for (int x = minX; x <= maxX; x+=voxelSizeInt) {
                    // Check intersection
                    // Simplified: just check if triangle is close to voxel center
                    // Or use a proper AABB-Tri test.
                    // For now, let's assume if it's in the bbox it's a candidate, 
                    // but we should be more precise to avoid blocky mess.
                    
                    // Let's just add it for now and refine later.
                    // Color sampling:
                    // Barycentric coords to get UV, then sample texture.
                    
                    // Sample texture
                    unsigned char r=255, g=255, b=255, a=255;
                    if (tri.materialIdx >= 0 && tri.materialIdx < model.materials.size()) {
                        const auto& mat = model.materials[tri.materialIdx];
                        // Base color
                        if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
                            r = mat.pbrMetallicRoughness.baseColorFactor[0] * 255;
                            g = mat.pbrMetallicRoughness.baseColorFactor[1] * 255;
                            b = mat.pbrMetallicRoughness.baseColorFactor[2] * 255;
                        }
                        // Texture
                        int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
                        if (texIdx >= 0 && texIdx < model.textures.size()) {
                            int imgIdx = model.textures[texIdx].source;
                            if (imgIdx >= 0 && imgIdx < model.images.size()) {
                                const auto& img = model.images[imgIdx];
                                if (!img.image.empty()) {
                                    // Sample UV (centroid of voxel? or triangle center?)
                                    // Using UV0 of triangle for simplicity (bad!)
                                    // Should interpolate UV at voxel center projected onto triangle.
                                    
                                    // Just use triangle vertex 0 UV for now to prove pipeline.
                                    int tx = (int)(tri.uv0.u * img.width) % img.width;
                                    int ty = (int)(tri.uv0.v * img.height) % img.height;
                                    if (tx < 0) tx += img.width;
                                    if (ty < 0) ty += img.height;
                                    
                                    int pixelIdx = (ty * img.width + tx) * img.component;
                                    if (pixelIdx + 2 < img.image.size()) {
                                        r = img.image[pixelIdx];
                                        g = img.image[pixelIdx+1];
                                        b = img.image[pixelIdx+2];
                                    }
                                }
                            }
                        }
                    }

                    grid.voxels.push_back({x, y, z, r, g, b, a});
                }
            }
        }
    }

    // Deduplicate voxels?
    // The current loop adds multiple voxels for overlapping triangles.
    // We should probably use a grid/map to store unique voxels.
    
    return grid;
}
