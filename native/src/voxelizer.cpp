#include "downloader.h"
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_ENABLE_DRACO         // <-- enable Draco
#define TINYGLTF_ENABLE_MESHOPT       // <-- strongly recommended, Google tiles use this too
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "voxelizer.h"
#include "../../../geoid.h"
#include <tiny_gltf.h>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>

// --- Helper Math ---

struct Vec3 { double x, y, z;
/*
	Vec3 &normalize()
	{
		double length = x * x + y * y + z * z;
		if (length == 0) // this check isn't an optimization but prevents getting NAN in the sqrt.
			return *this;
		length = core::reciprocal_squareroot(length);

		x = (z * length);
		y = (y * length);
		z = (z * length);
		return *this;
	}
*/

};
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

static inline float clamp01(float a)
{
	return a < 0 ? 0 : (a > 1 ? 1 : a);
}

static Vec3 ecefToLonLatHeight(const Vec3 &p)
{
	constexpr double a = 6378137.0;
	constexpr double f = 1.0 / 298.257223563;
	constexpr double e2 = f * (2.0 - f);
	constexpr double radToDeg = 180.0 / 3.14159265358979323846;

	const double lon = std::atan2(p.y, p.x);
	const double planar = std::sqrt(p.x * p.x + p.y * p.y);
	double lat = std::atan2(p.z, planar * (1.0 - e2));
	for (int i = 0; i < 5; ++i) {
		const double sinLat = std::sin(lat);
		const double n = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
		lat = std::atan2(p.z + e2 * n * sinLat, planar);
	}

	const double sinLat = std::sin(lat);
	const double n = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
	const double h = planar / std::cos(lat) - n;
	return {lon * radToDeg, lat * radToDeg, h};
}

// --- Voxelizer Implementation ---

#include "debug_log.h"
//#include "mapgen/earth/CpuVoxelizer.h"

bool triBoxOverlap(const Vec3& boxcenter, const Vec3& boxhalfsize, Vec3 triv0, Vec3 triv1, Vec3 triv2);

// ...



VoxelGrid Voxelizer::voxelize(const TileData &tile, int resolution, double originX,
		double originY, double originZ, double yOffsetNodes,
		double mapCenterLon, double mapCenterY, double mapCenterLat,
		double mapScaleX, double mapScaleY, double mapScaleZ,
		int nodeMinX, int nodeMinY, int nodeMinZ)
{
	VoxelGrid grid;
	tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

	const auto &glbData = tile.data;
	log_debug("[Voxelizer] Loading GLB (" + std::to_string(glbData.size()) + " bytes)");
	bool ret = loader.LoadBinaryFromMemory(&model, &err, &warn,
			reinterpret_cast<const unsigned char *>(glbData.data()), glbData.size());

	if (!warn.empty())
		log_debug("[Voxelizer] TinyGLTF Warn: " + warn);
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
	    bool hasUV;
	};
    std::vector<Triangle> triangles;

	using Mat4 = std::array<double, 16>;
	auto identityMat = []() {
		return Mat4{
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1
		};
	};
	auto multiplyMat = [](const Mat4 &a, const Mat4 &b) {
		Mat4 out{};
		for (int col = 0; col < 4; ++col) {
			for (int row = 0; row < 4; ++row) {
				for (int k = 0; k < 4; ++k)
					out[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
			}
		}
		return out;
	};
	auto transformPoint = [](const Mat4 &m, const Vec3 &v) {
		return Vec3{
			m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12],
			m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13],
			m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]
		};
	};
	auto tileTransform = identityMat();
	if (tile.transform.size() == 16) {
		for (size_t i = 0; i < 16; ++i)
			tileTransform[i] = tile.transform[i];
	}
	auto nodeLocalMatrix = [&](const tinygltf::Node &node) {
		Mat4 m = identityMat();
		if (node.matrix.size() == 16) {
			for (size_t i = 0; i < 16; ++i)
				m[i] = node.matrix[i];
			return m;
		}

		const double sx = node.scale.size() == 3 ? node.scale[0] : 1.0;
		const double sy = node.scale.size() == 3 ? node.scale[1] : 1.0;
		const double sz = node.scale.size() == 3 ? node.scale[2] : 1.0;

		Mat4 r = identityMat();
		if (node.rotation.size() == 4) {
			const double x = node.rotation[0], y = node.rotation[1];
			const double z = node.rotation[2], w = node.rotation[3];
			const double xx = x * x, yy = y * y, zz = z * z;
			const double xy = x * y, xz = x * z, yz = y * z;
			const double wx = w * x, wy = w * y, wz = w * z;
			r = Mat4{
				1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz),       2.0 * (xz - wy),       0,
				2.0 * (xy - wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx),       0,
				2.0 * (xz + wy),       2.0 * (yz - wx),       1.0 - 2.0 * (xx + yy), 0,
				0,                     0,                     0,                     1
			};
		}

		Mat4 s = identityMat();
		s[0] = sx; s[5] = sy; s[10] = sz;
		Mat4 t = identityMat();
		if (node.translation.size() == 3) {
			t[12] = node.translation[0];
			t[13] = node.translation[1];
			t[14] = node.translation[2];
		}
		return multiplyMat(t, multiplyMat(r, s));
	};

    // Helper to get buffer data
    auto getBuffer = [&](int accessorIdx) -> const unsigned char* {
        if (accessorIdx < 0) return nullptr;
        const auto& accessor = model.accessors[accessorIdx];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];
        return buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
    };

	std::function<void(int, const Mat4&)> gatherNode = [&](int nodeIdx, const Mat4 &parentMatrix) {
		if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size()))
			return;
		const auto& node = model.nodes[nodeIdx];
		const Mat4 nodeMatrix = multiplyMat(parentMatrix, nodeLocalMatrix(node));
        if (node.mesh >= 0) {
        const auto& mesh = model.meshes[node.mesh];

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
                return transformPoint(nodeMatrix, {
					(double)posBuffer[idx * posStride],
					(double)posBuffer[idx * posStride + 1],
					(double)posBuffer[idx * posStride + 2]
				});
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
					//DUMP(getPos(i0), getPos(i1), getPos(i2));
					triangles.push_back({getPos(i0), getPos(i1), getPos(i2), getUV(i0), getUV(i1), getUV(i2), primitive.material, uvBuffer != nullptr});
				}
            } else {
                for (size_t i = 0; i < vertexCount; i += 3) {
                    triangles.push_back({getPos(i), getPos(i+1), getPos(i+2), getUV(i), getUV(i+1), getUV(i+2), primitive.material, uvBuffer != nullptr});
                }
            }
        }
        }

		for (int childIdx : node.children)
			gatherNode(childIdx, nodeMatrix);
	};

	if (!model.scenes.empty()) {
		const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
		for (int nodeIdx : model.scenes[sceneIndex].nodes)
			gatherNode(nodeIdx, tileTransform);
	} else {
		for (size_t nodeIdx = 0; nodeIdx < model.nodes.size(); ++nodeIdx)
			gatherNode(static_cast<int>(nodeIdx), tileTransform);
	}

	Vec3 box_center{0, 0, 0};
	double box_size = 0;
	if (!tile.box.empty()) {
		box_center = {tile.box[0], tile.box[1], tile.box[2]};
		box_size = tile.box[11] * 2; // max?
	}

	// 2. Use Provided Origin (Global Origin)
	Vec3 center = { originX, originY, originZ };

	//center = box_center;

	double len = std::sqrt(center.x*center.x + center.y*center.y + center.z*center.z);
	if (len <= 0.0)
		return grid;
    Vec3 up = {center.x/len, center.y/len, center.z/len};
	Vec3 east{-up.y, up.x, 0.0};
	double eastLen = std::sqrt(east.x * east.x + east.y * east.y + east.z * east.z);
	if (eastLen < 1e-12) {
		east = {1.0, 0.0, 0.0};
		eastLen = 1.0;
	}
	east = east / eastLen;
	Vec3 north{
			up.y * east.z - up.z * east.y,
			up.z * east.x - up.x * east.z,
			up.x * east.y - up.y * east.x};

	auto dot3 = [](const Vec3 &a, const Vec3 &b) {
		return a.x * b.x + a.y * b.y + a.z * b.z;
	};
	auto ecefDeltaToLocal = [&](const Vec3 &delta) -> Vec3 {
		return {dot3(delta, east), dot3(delta, up), dot3(delta, north)};
	};

		Vec3 rotated_tile_box_center{0, 0, 0};
			if (!tile.box.empty())
			{
				const Vec3 tile_box_center = {tile.box[0], tile.box[1], tile.box[2]};
				rotated_tile_box_center = ecefDeltaToLocal(tile_box_center - center);
			}
	const auto rotated_half_center = rotated_tile_box_center / 2;
	const double gridCenter = resolution * 0.5;
	bool verticesAreEcef = false;
	if (!triangles.empty()) {
		const Vec3 &v = triangles.front().v0;
		const double vLen = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		verticesAreEcef = vLen > 1000000.0;
	}
	auto lengthSquared = [](const Vec3 &v) {
		return v.x * v.x + v.y * v.y + v.z * v.z;
	};
	auto gltfAxisToEcef = [](const Vec3 &v) {
		return Vec3{v.x, -v.z, v.y};
	};
	bool useGltfAxisToEcef = false;
	if (verticesAreEcef && !triangles.empty()) {
		const Vec3 &v = triangles.front().v0;
		const Vec3 reference = !tile.box.empty() ? box_center : center;
		const double rawD2 = lengthSquared(v - reference);
		const double swizzledD2 = lengthSquared(gltfAxisToEcef(v) - reference);
		useGltfAxisToEcef = swizzledD2 < rawD2;
	}
	const bool tileBoxCenterIsEcef = lengthSquared(box_center) > 1000000000000.0;
	const bool useGlobalMapProjection = (verticesAreEcef || tileBoxCenterIsEcef) &&
			std::isfinite(mapCenterLon) && std::isfinite(mapCenterLat) &&
			mapScaleX != 0.0 && mapScaleY != 0.0 && mapScaleZ != 0.0;
	auto vertexToVoxel = [&](const Vec3 &v) -> Vec3 {
		Vec3 world = useGltfAxisToEcef ? gltfAxisToEcef(v) : v;
		if (useGlobalMapProjection) {
			if (!verticesAreEcef)
				world = box_center + world;
			constexpr double metersPerDeg = 40075696.0 / 360.0;
			const Vec3 llh = ecefToLonLatHeight(world);
			const double mapX = ((llh.x - mapCenterLon) * metersPerDeg) / mapScaleX;
			const double orthometricHeight =
					earth::ellipsoid_to_orthometric_height(llh.y, llh.x, llh.z);
			const double mapY = orthometricHeight / mapScaleY - mapCenterY;
			const double mapZ = ((llh.y - mapCenterLat) * metersPerDeg) / mapScaleZ;
			return {mapX - nodeMinX, mapY - nodeMinY, mapZ - nodeMinZ};
		}
		const Vec3 local = verticesAreEcef ?
				ecefDeltaToLocal(world - center) :
				ecefDeltaToLocal(world) + rotated_tile_box_center;
		return {local.x + gridCenter, local.y + gridCenter, local.z + gridCenter};
	};

	for (auto& t : triangles) {
		t.v0 = vertexToVoxel(t.v0);
		t.v1 = vertexToVoxel(t.v1);
		t.v2 = vertexToVoxel(t.v2);
	}

		// Recompute BBox after rotation
		Vec3 min = {1e9, 1e9, 1e9}, max = {-1e9, -1e9, -1e9};
			for (const auto &t : triangles) {
				for (const auto &v : {t.v0, t.v1, t.v2}) {
				if (v.x < min.x)
					min.x = v.x;
				if (v.x > max.x)
					max.x = v.x;
				if (v.y < min.y)
					min.y = v.y;
				if (v.y > max.y)
					max.y = v.y;
				if (v.z < min.z)
					min.z = v.z;
				if (v.z > max.z)
					max.z = v.z;
				}
	    }

			if (std::isfinite(yOffsetNodes)) {
				for (auto &t : triangles) {
					t.v0.y += yOffsetNodes;
					t.v1.y += yOffsetNodes;
					t.v2.y += yOffsetNodes;
				}
				min.y += yOffsetNodes;
				max.y += yOffsetNodes;
			}

    // 3. Scan convert triangles, already in a common voxel coordinate frame.
    double maxDim = std::max({max.x - min.x, max.y - min.y, max.z - min.z});
	if (maxDim <= 0 || resolution <= 0)
		return grid;

	const double unit = 1.0;
	const double voxelSize = unit;

    Vec3 boxhalf{unit * 0.5, unit * 0.5, unit * 0.5};
    int nx = resolution;
    int ny = resolution;
    int nz = resolution;

    // Sparse grid map? Or dense if small enough.
    // Let's use a simple vector of voxels.
    
    // Very basic rasterizer: Check center of each voxel? Too slow (N^3).
    // Triangle-box intersection is better.

#if 0    
	for (const auto& tri : triangles) {
		// Compute triangle AABB in voxel grid space
		double tMinX = std::min({tri.v0.x, tri.v1.x, tri.v2.x});
		double tMaxX = std::max({tri.v0.x, tri.v1.x, tri.v2.x});
		double tMinY = std::min({tri.v0.y, tri.v1.y, tri.v2.y});
		double tMaxY = std::max({tri.v0.y, tri.v1.y, tri.v2.y});
		double tMinZ = std::min({tri.v0.z, tri.v1.z, tri.v2.z});
		double tMaxZ = std::max({tri.v0.z, tri.v1.z, tri.v2.z});

		int minX = std::max(0, (int)((tMinX - min.x) / voxelSize));
		int maxX = std::min(nx - 1, (int)((tMaxX - min.x) / voxelSize));
		int minY = std::max(0, (int)((tMinY - min.y) / voxelSize));
		int maxY = std::min(ny - 1, (int)((tMaxY - min.y) / voxelSize));
		int minZ = std::max(0, (int)((tMinZ - min.z) / voxelSize));
		int maxZ = std::min(nz - 1, (int)((tMaxZ - min.z) / voxelSize));

		// Iterate voxels in the triangle's AABB and test intersection
		for (int z = minZ; z <= maxZ; ++z) {
			for (int y = minY; y <= maxY; ++y) {
				for (int x = minX; x <= maxX; ++x) {
					Vec3 boxcenter = {
						min.x + (x + 0.5) * voxelSize,
						min.y + (y + 0.5) * voxelSize,
						min.z + (z + 0.5) * voxelSize
					};
					if (triBoxOverlap(boxcenter, boxhalf, tri.v0, tri.v1, tri.v2)) {
						unsigned char r = 255, g = 255, b = 255, a = 255;
						grid.voxels.push_back({x, y, z, r, g, b, a});
					}
				}
			}
		}
	}
#endif
if (0)
   for (const auto& tri : triangles) {
// /*
     	for (const auto &t : {tri.v0, tri.v1, tri.v2}) {
			unsigned char r = 100, g = 100, b = 100, a = 100;

			if (static int i = 0; !((++i)%1000))
			//DUMP(t);
			grid.voxels.push_back({static_cast<int>(t.x), static_cast<int>(t.y),
					static_cast<int>(t.z), r, g, b, a});
		}
		//continue;
// */
				// BBox of triangle
				double tMinX = std::min({tri.v0.x, tri.v1.x, tri.v2.x});
				double tMaxX = std::max({tri.v0.x, tri.v1.x, tri.v2.x});
				double tMinY = std::min({tri.v0.y, tri.v1.y, tri.v2.y});
				double tMaxY = std::max({tri.v0.y, tri.v1.y, tri.v2.y});
				double tMinZ = std::min({tri.v0.z, tri.v1.z, tri.v2.z});
				double tMaxZ = std::max({tri.v0.z, tri.v1.z, tri.v2.z});

				/*
				int minX = std::max(0, (int)((tMinX - min.x) / voxelSize));
				int maxX = std::min(nx - 1, (int)((tMaxX - min.x) / voxelSize));
				int minY = std::max(0, (int)((tMinY - min.y) / voxelSize));
				int maxY = std::min(ny - 1, (int)((tMaxY - min.y) / voxelSize));
				int minZ = std::max(0, (int)((tMinZ - min.z) / voxelSize));
				int maxZ = std::min(nz - 1, (int)((tMaxZ - min.z) / voxelSize));
                */

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
					Vec3 boxcenter = {
						min.x + (x + 0.5) * voxelSizeInt,
						min.y + (y + 0.5) * voxelSizeInt,
						min.z + (z + 0.5) * voxelSizeInt
					};
//DUMP(x,y,z, boxcenter, voxelSize);

					//if (!triBoxOverlap(boxcenter, boxhalf, tri.v0, tri.v1, tri.v2)) continue;
					
//DUMP("place", x,y,z, boxcenter, voxelSize);
							// Let's just add it for now and refine later.
							// Color sampling:
							// Barycentric coords to get UV, then sample texture.

							// Sample texture
							unsigned char r = 255, g = 255, b = 255, a = 255;
							bool have = false;
							if (tri.materialIdx >= 0 &&
									tri.materialIdx < model.materials.size()) {
								const auto &mat = model.materials[tri.materialIdx];
								// Base color
								if (mat.pbrMetallicRoughness.baseColorFactor.size() ==
										4) {
									r = mat.pbrMetallicRoughness.baseColorFactor[0] * 255;
									g = mat.pbrMetallicRoughness.baseColorFactor[1] * 255;
									b = mat.pbrMetallicRoughness.baseColorFactor[2] * 255;
									have = true;
								}
								// Texture
								int texIdx =
										mat.pbrMetallicRoughness.baseColorTexture.index;
								if (texIdx >= 0 && texIdx < model.textures.size()) {
									int imgIdx = model.textures[texIdx].source;
									if (imgIdx >= 0 && imgIdx < model.images.size()) {
										const auto &img = model.images[imgIdx];
										if (!img.image.empty()) {
											// Sample UV (centroid of voxel? or triangle center?)
											// Using UV0 of triangle for simplicity (bad!)
											// Should interpolate UV at voxel center projected onto triangle.

											// Just use triangle vertex 0 UV for now to prove pipeline.
											int tx = (int)(tri.uv0.u * img.width) %
													 img.width;
											int ty = (int)(tri.uv0.v * img.height) %
													 img.height;
											if (tx < 0)
												tx += img.width;
											if (ty < 0)
												ty += img.height;

											int pixelIdx =
													(ty * img.width + tx) * img.component;
											if (pixelIdx + 2 < img.image.size()) {
												r = img.image[pixelIdx];
												g = img.image[pixelIdx + 1];
												b = img.image[pixelIdx + 2];
												have = true;
											}
										}
									}
								}
							}
							//if (have)
								grid.voxels.push_back({x, y, z, r, g, b, a});
						}
					}
				}
			}

{

static const float NEAR_W_FRACTION = 0.15f; // second‑slice threshold
	const float nearFrac = NEAR_W_FRACTION;

bool 	flipV = false;
	(void)flipV;

	//const auto G = maxDim + 1;
	const auto G = resolution;

	const auto grid_ = G;
	int total = grid_ * grid_ * grid_;
	std::vector<uint8_t> occ(total, 0);
	std::vector<std::array<uint8_t, 4>> colors(total, {255, 255, 255, 255});
	std::vector<double> bestD2(total, std::numeric_limits<double>::infinity());


	const auto z0 = 0;
	const auto z1 = G;

	static const float EPS = 1e-6f; // inside test epsilon
	constexpr auto &eps = EPS;
	auto sampleImage = [](const tinygltf::Image &img, double u, double v) {
		std::array<uint8_t, 4> out{255, 255, 255, 255};
		if (img.width <= 0 || img.height <= 0 || img.component <= 0 || img.image.empty())
			return out;

		u = std::clamp(u, 0.0, 1.0);
		v = std::clamp(v, 0.0, 1.0);
		const double fx = u * (img.width - 1);
		const double fy = v * (img.height - 1);
		const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, img.width - 1);
		const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, img.height - 1);
		const int x1 = std::min(x0 + 1, img.width - 1);
		const int y1 = std::min(y0 + 1, img.height - 1);
		const double dx = fx - x0;
		const double dy = fy - y0;

		auto texel = [&](int x, int y) {
			std::array<double, 3> c{255.0, 255.0, 255.0};
			const size_t p = (static_cast<size_t>(y) * img.width + x) * img.component;
			if (p >= img.image.size())
				return c;
			c[0] = img.image[p];
			c[1] = img.component >= 3 && p + 1 < img.image.size() ? img.image[p + 1] : c[0];
			c[2] = img.component >= 3 && p + 2 < img.image.size() ? img.image[p + 2] : c[0];
			return c;
		};

		const auto c00 = texel(x0, y0);
		const auto c10 = texel(x1, y0);
		const auto c01 = texel(x0, y1);
		const auto c11 = texel(x1, y1);
		const double w00 = (1.0 - dx) * (1.0 - dy);
		const double w10 = dx * (1.0 - dy);
		const double w01 = (1.0 - dx) * dy;
		const double w11 = dx * dy;
		for (int i = 0; i < 3; ++i) {
			out[i] = static_cast<uint8_t>(std::clamp<int>(
					static_cast<int>(c00[i] * w00 + c10[i] * w10 +
							c01[i] * w01 + c11[i] * w11),
					0, 255));
		}
		return out;
	};

	auto sampleTriangleColor = [&](const Triangle &tri, double l0, double l1, double l2) {
		std::array<uint8_t, 4> color{255, 255, 255, 255};
		if (tri.materialIdx >= 0 && tri.materialIdx < static_cast<int>(model.materials.size())) {
			const auto &mat = model.materials[tri.materialIdx];
			if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
				color[0] = static_cast<uint8_t>(std::clamp<int>(
						static_cast<int>(mat.pbrMetallicRoughness.baseColorFactor[0] * 255.0), 0, 255));
				color[1] = static_cast<uint8_t>(std::clamp<int>(
						static_cast<int>(mat.pbrMetallicRoughness.baseColorFactor[1] * 255.0), 0, 255));
				color[2] = static_cast<uint8_t>(std::clamp<int>(
						static_cast<int>(mat.pbrMetallicRoughness.baseColorFactor[2] * 255.0), 0, 255));
			}
			const int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
			if (tri.hasUV && texIdx >= 0 && texIdx < static_cast<int>(model.textures.size())) {
				const int imgIdx = model.textures[texIdx].source;
				if (imgIdx >= 0 && imgIdx < static_cast<int>(model.images.size())) {
					const double su = l0 * tri.uv0.u + l1 * tri.uv1.u + l2 * tri.uv2.u;
					const double sv = l0 * tri.uv0.v + l1 * tri.uv1.v + l2 * tri.uv2.v;
					color = sampleImage(model.images[imgIdx], su, sv);
				}
			}
		}
		return color;
	};

	for (const auto &tri : triangles) {
			const double x0 = tri.v0.x, y0 = tri.v0.y, z00 = tri.v0.z;
			const double x1 = tri.v1.x, y1 = tri.v1.y, z1f = tri.v1.z;
			const double x2 = tri.v2.x, y2 = tri.v2.y, z2f = tri.v2.z;

			const Vec3 e10{x1 - x0, y1 - y0, z1f - z00};
			const Vec3 e20{x2 - x0, y2 - y0, z2f - z00};
			const Vec3 n{
					e10.y * e20.z - e10.z * e20.y,
					e10.z * e20.x - e10.x * e20.z,
					e10.x * e20.y - e10.y * e20.x};
			const double nLen2 = n.x * n.x + n.y * n.y + n.z * n.z;
			if (nLen2 < 1e-20)
				continue;

				// AABB in voxel space. Reject before clamping so neighboring
				// chunks are not smeared onto this chunk's border.
				const int rawMinX = static_cast<int>(std::floor(std::min({x0, x1, x2})));
				const int rawMinY = static_cast<int>(std::floor(std::min({y0, y1, y2})));
				const int rawMinZ = static_cast<int>(std::floor(std::min({z00, z1f, z2f})));
				const int rawMaxX = static_cast<int>(std::ceil(std::max({x0, x1, x2})));
				const int rawMaxY = static_cast<int>(std::ceil(std::max({y0, y1, y2})));
				const int rawMaxZ = static_cast<int>(std::ceil(std::max({z00, z1f, z2f})));
				if (rawMaxX < 0 || rawMinX >= G || rawMaxY < 0 || rawMinY >= G ||
						rawMaxZ < z0 || rawMinZ >= z1)
					continue;
				int minX = std::clamp<int>(rawMinX, 0, G - 1);
				int minY = std::clamp<int>(rawMinY, 0, G - 1);
				int minZ = std::clamp<int>(rawMinZ, 0, G - 1);
				int maxX = std::clamp<int>(rawMaxX, 0, G - 1);
				int maxY = std::clamp<int>(rawMaxY, 0, G - 1);
				int maxZ = std::clamp<int>(rawMaxZ, 0, G - 1);

			// Dominant axis
			double abx = std::abs(n.x), aby = std::abs(n.y), abz = std::abs(n.z);
			int wAxis;
			if (abz >= abx && abz >= aby) {
				wAxis = 2;
			} else if (aby >= abx) {
				wAxis = 1;
			} else {
				wAxis = 0;
			}

		// Map to (U,V,W)
		double U0, V0, W0, U1, V1, W1, U2, V2, W2;
		if (wAxis == 2) { // Z‑major
			U0 = x0;
			V0 = y0;
			W0 = z00;
			U1 = x1;
			V1 = y1;
			W1 = z1f;
			U2 = x2;
			V2 = y2;
			W2 = z2f;
		} else if (wAxis == 1) { // Y‑major
			U0 = z00;
			V0 = x0;
			W0 = y0;
			U1 = z1f;
			V1 = x1;
			W1 = y1;
			U2 = z2f;
			V2 = x2;
			W2 = y2;
		} else { // X‑major
			U0 = y0;
			V0 = z00;
			W0 = x0;
			U1 = y1;
			V1 = z1f;
			W1 = x1;
			U2 = y2;
			V2 = z2f;
			W2 = x2;
		}

		// 2D bbox on (U,V)
		int uMinRaw = static_cast<int>(std::floor(std::min({U0, U1, U2})));
		int vMinRaw = static_cast<int>(std::floor(std::min({V0, V1, V2})));
		int uMaxRaw = static_cast<int>(std::ceil(std::max({U0, U1, U2})));
		int vMaxRaw = static_cast<int>(std::ceil(std::max({V0, V1, V2})));
		int uMin = std::clamp<int>(uMinRaw, 0, G - 1);
		int vMin = std::clamp<int>(vMinRaw, 0, G - 1);
		int uMax = std::clamp<int>(uMaxRaw, 0, G - 1);
		int vMax = std::clamp<int>(vMaxRaw, 0, G - 1);
		if (uMin > uMax || vMin > vMax)
			continue;

		// Barycentric gradients
		double denom = (V1 - V2) * (U0 - U2) + (U2 - U1) * (V0 - V2);
		if (std::abs(denom) < 1e-12f)
			continue;
		double invDen = 1.0 / denom;
		double dL0du = (V1 - V2) * invDen;
		double dL1du = (V2 - V0) * invDen;

		// W interpolation coefficients
		double Wc0 = W0 - W2;
		double Wc1 = W1 - W2;
		double WcC = W2;

		// Normal w‑component for depth scale
		double nW = (wAxis == 2 ? n.z : (wAxis == 1 ? n.y : n.x));
		double depthScale = (nW * nW) / nLen2;

		// Scan rows in V
		for (int v = vMin; v <= vMax; ++v) {
			double u0c = uMin + 0.5;
			double vc = v + 0.5;

			// L0, L1 at (u0c, vc)
			double L0 = ((V1 - V2) * (u0c - U2) + (U2 - U1) * (vc - V2)) * invDen;
			double L1 = ((V2 - V0) * (u0c - U2) + (U0 - U2) * (vc - V2)) * invDen;
			double L2 = 1.0 - L0 - L1;

			// W at start of row and dW/du
			double W = Wc0 * L0 + Wc1 * L1 + WcC;
			double dWdu = dL0du * Wc0 + dL1du * Wc1;

			double L0du = dL0du, L1du = dL1du;

			for (int u = uMin; u <= uMax; ++u) {
				if (L0 >= -eps && L1 >= -eps && L2 >= -eps) {
					int ix, iy, iz;
					int wIdx = static_cast<int>(std::floor(W));

					// Map back to (x,y,z)
					if (wAxis == 2) {
						ix = u;
						iy = v;
						iz = wIdx;
					} else if (wAxis == 1) {
						ix = v;
						iy = wIdx;
						iz = u;
					} else {
						ix = wIdx;
						iy = u;
						iz = v;
					}

					//if (iz >= z0 && iz < z1 && ix >= 0 && ix < G && iy >= 0 && iy < G) {
					if (iz >= z0 && iz < z1 && ix >= 0 && ix < G && iy >= 0 && iy < G) {
						int lin = ix + G * (iy + G * iz);
						double delta =
								W - ((wAxis == 2 ? iz : (wAxis == 1 ? iy : ix)) + 0.5);
						double d2 = delta * delta * depthScale;
//DUMP(ix, iy, iz, lin);
						if (d2 < bestD2[lin]) {
							bestD2[lin] = d2;
							occ[lin] = 1;
							colors[lin] = sampleTriangleColor(tri, L0, L1, L2);
						}
					}

					// Near‑slice handling
					double frac = W - std::floor(W);
					if (frac < nearFrac || frac > 1.0f - nearFrac) {
						int w2 = ((W - (wIdx + 0.5)) < 0) ? (wIdx - 1) : (wIdx + 1);

						int ix2, iy2, iz2;
						if (wAxis == 2) {
							ix2 = u;
							iy2 = v;
							iz2 = w2;
						} else if (wAxis == 1) {
							ix2 = v;
							iy2 = w2;
							iz2 = u;
						} else {
							ix2 = w2;
							iy2 = u;
							iz2 = v;
						}

						if (iz2 >= z0 && iz2 < z1 && ix2 >= 0 && ix2 < G && iy2 >= 0 &&
								iy2 < G && w2 >= 0 && w2 < G) {
							int lin2 = ix2 + G * (iy2 + G * iz2);
							double delta2 = W - (static_cast<double>(w2) + 0.5);
							double d2b = delta2 * delta2 * depthScale;
							if (d2b < bestD2[lin2]) {
								bestD2[lin2] = d2b;
								occ[lin2] = 1;
								colors[lin2] = sampleTriangleColor(tri, L0, L1, L2);
							}
						}
					}
				}

				// advance u
				L0 += L0du;
				L1 += L1du;
				L2 = 1.0f - L0 - L1;
				W += dWdu;
			}
		}
	}

	grid.voxels.reserve(grid.voxels.size() + total / 16);
	for (int i = 0; i < total; ++i) {
		if (!occ[i])
			continue;
		const int z = i / (G * G);
		const int rem = i - z * G * G;
		const int y = rem / G;
		const int x = rem - y * G;
		const auto &c = colors[i];
		grid.voxels.push_back({x, y, z, c[0], c[1], c[2], c[3]});
	}
}

	// Deduplicate voxels?
    // The current loop adds multiple voxels for overlapping triangles.
    // We should probably use a grid/map to store unique voxels.
    
    return grid;
}
