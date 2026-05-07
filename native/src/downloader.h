#pragma once
#include <string>
#include <vector>
#include <mutex>

struct TileData {
    std::string url;
    std::string data;
    std::vector<double> box;
    std::vector<double> transform;
    double geometricError = 0.0;
    // Add metadata like translation, etc.
};

class TileDownloader {
public:
    TileDownloader(const std::string& apiKey, const std::string& cacheDir = "");
    
    // Downloads tiles intersecting the region
    std::vector<TileData> downloadTiles(double lat, double lon, double elevation, double radius);
    
    std::string fetchUrlPublic(const std::string& url);

private:
    std::string apiKey;
    std::string cacheDir;
    static std::mutex cacheMutex;
    
    // Helper to fetch a URL, returns (data, content-type)
    std::pair<std::string, std::string> fetchUrl(const std::string& url);
    std::pair<std::string, std::string> fetchUrlNonCached(const std::string& url);
};
