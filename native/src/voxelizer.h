#pragma once
#include <cstdint>
#include <functional>
#include <vector>

struct TileData;

struct Voxel
{
	int x, y, z;
	uint8_t r, g, b, a;
};

struct VoxelGrid
{
	std::vector<Voxel> voxels;
	// Add bounds, scale, etc.
};

class Voxelizer
{
public:
	VoxelGrid voxelize(const TileData &tile, int resolution, double originX,
			double originY, double originZ);
};

using callback_t = std::function<void(const int &x, const int &y, const int &z,
		const uint8_t &r, const uint8_t &g, const uint8_t &b, const uint8_t &a)>;
