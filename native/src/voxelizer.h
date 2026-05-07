#pragma once
#include <cstdint>
#include <functional>
#include <limits>
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
			double originY, double originZ,
			double yOffsetNodes = std::numeric_limits<double>::quiet_NaN(),
			double mapCenterLon = std::numeric_limits<double>::quiet_NaN(),
			double mapCenterY = 0.0,
			double mapCenterLat = std::numeric_limits<double>::quiet_NaN(),
			double mapScaleX = 1.0,
			double mapScaleY = 1.0,
			double mapScaleZ = 1.0,
			int nodeMinX = 0,
			int nodeMinY = 0,
			int nodeMinZ = 0);
};

using callback_t = std::function<void(const int &x, const int &y, const int &z,
		const uint8_t &r, const uint8_t &g, const uint8_t &b, const uint8_t &a)>;
