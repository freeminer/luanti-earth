#include "downloader.h"
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

static inline float clamp01(float a)
{
	return a < 0 ? 0 : (a > 1 ? 1 : a);
}

// --- Voxelizer Implementation ---

#include "debug_log.h"

bool triBoxOverlap(const Vec3& boxcenter, const Vec3& boxhalfsize, Vec3 triv0, Vec3 triv1, Vec3 triv2);

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

    // 3. Rasterize (Simple 3D point-in-tri or conservative rasterization)
    // For simplicity, let's do a basic grid traversal or point sampling.
    // Given the resolution (e.g. 200), we define voxel size.
    
    double maxDim = std::max({max.x - min.x, max.y - min.y, max.z - min.z});
	double voxelSize = maxDim / box_size;
	if (voxelSize <= 0)
		return grid;

    Vec3 boxhalf{voxelSize*0.5, voxelSize*0.5, voxelSize*0.5};
    int nx = std::ceil((max.x - min.x) / voxelSize);
    int ny = std::ceil((max.y - min.y) / voxelSize);
    int nz = std::ceil((max.z - min.z) / voxelSize);

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
			DUMP(t);
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
			if (static int i = 0; !((++i)%10000))
			DUMP(x, y, z, r, g, b, a);

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

	//const auto G = maxDim + 1;
	const auto G = resolution;

	const auto grid_ = G;
	int total = grid_ * grid_ * grid_;
	std::vector<bool> occ(total, false);
	//std::vector<uint32_t> colors(total, 0xFFFFFF);
//DUMP(grid_, total);
	std::vector<float> bestD2(total, std::numeric_limits<float>::infinity());


	const auto z0 = 0;
	const auto z1 = maxDim;

	static const float EPS = 1e-6f; // inside test epsilon
	constexpr auto &eps = EPS;
	for (const auto &tri : triangles) {

			if (static int i = 0; !((++i)%1000))
			DUMP(tri.v0, tri.v1,tri.v2, tri.uv0, tri.materialIdx);

			const auto pick_color_and_return = [&](const int &x, const int &y,
													   const int &z) {
				// Let's just add it for now and refine later.
				// Color sampling:
				// Barycentric coords to get UV, then sample texture.

				// Sample texture
				unsigned char r = 255, g = 255, b = 255, a = 255;
				bool have = false;
				if (tri.materialIdx >= 0 && tri.materialIdx < model.materials.size()) {
					const auto &mat = model.materials[tri.materialIdx];
					// Base color
					if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
						r = mat.pbrMetallicRoughness.baseColorFactor[0] * 255;
						g = mat.pbrMetallicRoughness.baseColorFactor[1] * 255;
						b = mat.pbrMetallicRoughness.baseColorFactor[2] * 255;
						have = true;
					}
					// Texture
					int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
					if (texIdx >= 0 && texIdx < model.textures.size()) {
						int imgIdx = model.textures[texIdx].source;
						if (imgIdx >= 0 && imgIdx < model.images.size()) {
							const auto &img = model.images[imgIdx];
							if (!img.image.empty()) {
								// Sample UV (centroid of voxel? or triangle center?)
								// Using UV0 of triangle for simplicity (bad!)
								// Should interpolate UV at voxel center projected onto triangle.

								// Just use triangle vertex 0 UV for now to prove pipeline.
								int tx = (int)(tri.uv0.u * img.width) % img.width;
								int ty = (int)(tri.uv0.v * img.height) % img.height;
								if (tx < 0)
									tx += img.width;
								if (ty < 0)
									ty += img.height;

								int pixelIdx = (ty * img.width + tx) * img.component;
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
				if (static int i = 0; !((++i) % 10000)) {
					DUMP(x, y, z, r, g, b, a);
				}
				grid.voxels.push_back({x, y, z, r, g, b, a});
			};

			//for (size_t ii = 0; ii < triCount; ++ii) {
			//	int i = triIdx[ii];

			// Vertices
			// float x0 = T.x0[i], y0 = T.y0[i], z00 = T.z0[i];
			// float x1 = T.x1[i], y1 = T.y1[i], z1f = T.z1[i];
			// float x2 = T.x2[i], y2 = T.y2[i], z2f = T.z2[i];
			auto &x0 = tri.v0.x, &y0 = tri.v0.y, &z00 = tri.v0.z;
			auto &x1 = tri.v1.x, &y1 = tri.v1.y, &z1f = tri.v1.z;
			auto &x2 = tri.v2.x, &y2 = tri.v2.y, &z2f = tri.v2.z;

			// Normal & edges
			// float e10x = T.e10x[i], e10y = T.e10y[i], e10z = T.e10z[i];
			// float e20x = T.e20x[i], e20y = T.e20y[i], e20z = T.e20z[i];
			// float nx = T.nx[i], ny = T.ny[i], nz = T.nz[i];

			// AABB in voxel space
			int minX = std::clamp<int>(
					static_cast<int>(std::floor(std::min({x0, x1, x2}))), 0, G - 1);
			int minY = std::clamp<int>(
					static_cast<int>(std::floor(std::min({y0, y1, y2}))), 0, G - 1);
			int minZ = std::clamp<int>(
					static_cast<int>(std::floor(std::min({z00, z1f, z2f}))), 0, G - 1);
			int maxX = std::clamp<int>(
					static_cast<int>(std::ceil(std::max({x0, x1, x2}))), 0, G - 1);
			int maxY = std::clamp<int>(
					static_cast<int>(std::ceil(std::max({y0, y1, y2}))), 0, G - 1);
			int maxZ = std::clamp<int>(
					static_cast<int>(std::ceil(std::max({z00, z1f, z2f}))), 0, G - 1);

			// Skip if slab does not intersect
			//if (maxZ < z0 || minZ >= z1)
			//	continue;

			// Dominant axis
			float abx = std::abs(nx), aby = std::abs(ny), abz = std::abs(nz);
			int wAxis, uAxis, vAxis;
			if (abz >= abx && abz >= aby) {
				wAxis = 2;
				uAxis = 0;
				vAxis = 1;
		} else if (aby >= abx) {
			wAxis = 1;
			uAxis = 2;
			vAxis = 0;
		} else {
			wAxis = 0;
			uAxis = 1;
			vAxis = 2;
		}

		// Map to (U,V,W)
		float U0, V0, W0, U1, V1, W1, U2, V2, W2;
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
		float denom = (V1 - V2) * (U0 - U2) + (U2 - U1) * (V0 - V2);
		if (std::abs(denom) < 1e-12f)
			continue;
		float invDen = 1.0f / denom;
		float dL0du = (V1 - V2) * invDen;
		float dL0dv = (U2 - U1) * invDen;
		float dL1du = (V2 - V0) * invDen;
		float dL1dv = (U0 - U2) * invDen;

		// W interpolation coefficients
		float Wc0 = W0 - W2;
		float Wc1 = W1 - W2;
		float WcC = W2;

		// Normal w‑component for depth scale
		float nW = (wAxis == 2 ? nz : (wAxis == 1 ? ny : nx));
		float nLen2 = nx * nx + ny * ny + nz * nz;
		if (nLen2 < 1e-20f)
			continue;
		float depthScale = (nW * nW) / nLen2;

		// UVs
		float tu0 = tri.uv0.u, tv0 = tri.uv0.v;
		float tu1 = tri.uv1.u, tv1 = tri.uv1.v;
		float tu2 = tri.uv2.u, tv2 = tri.uv2.v;

		bool hasUV = true; // TODOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
		//T.hasUV[i] &&
		//tex != nullptr;

		// Scan rows in V
		for (int v = vMin; v <= vMax; ++v) {
			float u0c = uMin + 0.5f;
			float vc = v + 0.5f;

			// L0, L1 at (u0c, vc)
			float L0 = ((V1 - V2) * (u0c - U2) + (U2 - U1) * (vc - V2)) * invDen;
			float L1 = ((V2 - V0) * (u0c - U2) + (U0 - U2) * (vc - V2)) * invDen;
			float L2 = 1.0f - L0 - L1;

			// W at start of row and dW/du
			float W = Wc0 * L0 + Wc1 * L1 + WcC;
			float dWdu = dL0du * Wc0 + dL1du * Wc1;

			float L0du = dL0du, L1du = dL1du;

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
						float delta =
								W - ((wAxis == 2 ? iz : (wAxis == 1 ? iy : ix)) + 0.5f);
						float d2 = delta * delta * depthScale;
//DUMP(ix, iy, iz, lin);
						if (d2 < bestD2[lin]) {
							bestD2[lin] = d2;
							occ[lin] = true;
							//!!!!!!!colors[lin] =
							if (hasUV) {
								pick_color_and_return(ix, iy, iz);
								/*
								const auto col = sampleCUDA(T.texPixels, texW, texH,
										texStride,
										clamp01(L0 * tu0 + L1 * tu1 + L2 * tu2),
										clamp01(L0 * tv0 + L1 * tv1 + L2 * tv2), flipV);
								if (col[3]) {
									callback(ix, iy, iz, col[0], col[1], col[2], col[3]);
									//++filledHere;
								}
									*/
							}
						}
					}

					// Near‑slice handling
					float frac = W - static_cast<float>(std::floor(W));
					if (frac < nearFrac || frac > 1.0f - nearFrac) {
						int w2 = ((W - (wIdx + 0.5f)) < 0) ? (wIdx - 1) : (wIdx + 1);

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
							float delta2 = W - (static_cast<float>(w2) + 0.5f);
							float d2b = delta2 * delta2 * depthScale;
							if (d2b < bestD2[lin2]) {
								bestD2[lin2] = d2b;
								occ[lin2] = true;
								//!!!!!!colors[lin2] =
								if (hasUV) {
									pick_color_and_return(ix, iy, iz);
									/*
									const auto col = sampleCUDA(T.texPixels, texW, texH,
											texStride,
											clamp01(L0 * tu0 + L1 * tu1 + L2 * tu2),
											clamp01(L0 * tv0 + L1 * tv1 + L2 * tv2),
											flipV);
									if (col[3]) {
										callback(ix, iy, iz, col[0], col[1], col[2],
												col[3]);
										//++filledHere;
										}
*/
								}
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
		//}
	}
}

    // Deduplicate voxels?
    // The current loop adds multiple voxels for overlapping triangles.
    // We should probably use a grid/map to store unique voxels.
    
    return grid;
}
