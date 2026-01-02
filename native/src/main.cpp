#include <iostream>
#include <thread>
#include <atomic>
#include <cmath>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <cstring>

#include "downloader.h"
#include "voxelizer.h"
#include "debug_log.h"

// Helper to convert lat/lon to ECEF
struct Vec3 { double x, y, z; };
static Vec3 cartesianFromDegrees(double lat, double lon, double h = 0) {
    const double a = 6378137.0;
    const double f = 1.0 / 298.257223563;
    const double e2 = f * (2.0 - f);
    const double radLat = lat * 3.14159265358979323846 / 180.0;
    const double radLon = lon * 3.14159265358979323846 / 180.0;
    const double sinLat = std::sin(radLat);
    const double cosLat = std::cos(radLat);
    const double N = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
    const double x = (N + h) * cosLat * std::cos(radLon);
    const double y = (N + h) * cosLat * std::sin(radLon);
    const double z = (N * (1.0 - e2) + h) * sinLat;
    return { x, y, z };
}

// --- Job System ---

struct Job {
    std::atomic<int> status{0}; // 0=running, 1=done, -1=error
    std::vector<char> result;
    std::string error_msg;
};

static std::mutex g_jobs_mutex;
static std::map<int, std::shared_ptr<Job>> g_jobs;
static std::atomic<int> g_nextJobId{1};

extern "C" {

#ifdef _WIN32
    #define EXPORT __declspec(dllexport)
#else
    #define EXPORT
#endif

    EXPORT int start_download_and_voxelize(double lat, double lon, double radius, int resolution, const char* api_key) {
        int jobId = g_nextJobId++;
        auto job = std::make_shared<Job>();
        
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            g_jobs[jobId] = job;
        }

        std::string apiKeyStr = (api_key ? api_key : "");

        std::thread([job, lat, lon, radius, resolution, apiKeyStr]() {
            try {
                log_debug("[Job] Starting download for lat=" + std::to_string(lat) + " lon=" + std::to_string(lon));
                
                TileDownloader downloader(apiKeyStr);
                auto tiles = downloader.downloadTiles(lat, lon, 0, radius);
                log_debug("[Job] Downloaded " + std::to_string(tiles.size()) + " tiles");

                Voxelizer voxelizer;
                Vec3 origin = cartesianFromDegrees(lat, lon, 0);

                std::vector<char> buffer;
                buffer.reserve(10 * 1024 * 1024); // Reserve 10MB

                for (size_t i = 0; i < tiles.size(); ++i) {
                    const auto& tile = tiles[i];
                    VoxelGrid grid = voxelizer.voxelize(tile.data, resolution, origin.x, origin.y, origin.z);
                    
                    for (const auto& v : grid.voxels) {
                        // Append x, y, z (int32 little endian)
                        int32_t x = v.x;
                        int32_t y = v.y;
                        int32_t z = v.z;
                        
                        const char* px = reinterpret_cast<const char*>(&x);
                        buffer.insert(buffer.end(), px, px + 4);
                        
                        const char* py = reinterpret_cast<const char*>(&y);
                        buffer.insert(buffer.end(), py, py + 4);
                        
                        const char* pz = reinterpret_cast<const char*>(&z);
                        buffer.insert(buffer.end(), pz, pz + 4);

                        // Append r, g, b, a (uint8)
                        buffer.push_back((char)v.r);
                        buffer.push_back((char)v.g);
                        buffer.push_back((char)v.b);
                        buffer.push_back((char)v.a);
                    }
                }

                log_debug("[Job] Finished. Buffer size: " + std::to_string(buffer.size()));
                job->result = std::move(buffer);
                job->status = 1; // Done
            } catch (const std::exception& e) {
                log_debug("[Job] Error: " + std::string(e.what()));
                job->error_msg = e.what();
                job->status = -1; // Error
            } catch (...) {
                log_debug("[Job] Unknown error");
                job->error_msg = "Unknown error";
                job->status = -1;
            }
        }).detach();

        return jobId;
    }

    EXPORT int get_job_status(int job_id) {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) return -2; // Invalid job
        return it->second->status;
    }

    EXPORT int get_job_result_size(int job_id) {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) return 0;
        if (it->second->status != 1) return 0;
        return (int)it->second->result.size();
    }

    EXPORT int get_job_result(int job_id, char* buffer, int max_len) {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(job_id);
        if (it == g_jobs.end()) return 0;
        
        const auto& res = it->second->result;
        int to_copy = std::min(max_len, (int)res.size());
        if (to_copy > 0) {
            std::memcpy(buffer, res.data(), to_copy);
        }
        return to_copy;
    }

    EXPORT void free_job(int job_id) {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        g_jobs.erase(job_id);
    }
}
