#pragma once
#include <string>
#include <vector>
#include <mutex>

struct TileData {
    std::string url;
    std::vector<unsigned char> data;
    // Add metadata like translation, etc.
};

class TileDownloader {
public:
    TileDownloader(const std::string& apiKey, const std::string& cacheDir = "");
    
    // Downloads tiles intersecting the region
    std::vector<TileData> downloadTiles(double lat, double lon, double radius);
    
    std::vector<unsigned char> fetchUrlPublic(const std::string& url);

private:
    std::string apiKey;
    std::string cacheDir;
    static std::mutex cacheMutex;
    
    // Helper to fetch a URL, returns (data, content-type)
    std::pair<std::vector<unsigned char>, std::string> fetchUrl(const std::string& url);
    std::pair<std::vector<unsigned char>, std::string> fetchUrlNonCached(const std::string& url);
};
