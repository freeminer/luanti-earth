#include "downloader.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <cmath>
#include <string>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

std::mutex TileDownloader::cacheMutex;

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#endif

using json = nlohmann::json;

// --- Helper Classes for Geometry ---

struct Vector3 {
    double x, y, z;
};

struct Sphere {
    Vector3 center;
    double radius;

    bool intersects(const Sphere& other) const {
        double dx = other.center.x - center.x;
        double dy = other.center.y - center.y;
        double dz = other.center.z - center.z;
        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        return dist < (radius + other.radius);
    }
};

// Convert degrees to Cartesian (ECEF approx)
Vector3 cartesianFromDegrees(double lonDeg, double latDeg, double h = 0) {
    const double a = 6378137.0;
    const double f = 1.0 / 298.257223563;
    const double e2 = f * (2.0 - f);
    const double radLat = latDeg * 3.14159265358979323846 / 180.0;
    const double radLon = lonDeg * 3.14159265358979323846 / 180.0;
    const double sinLat = std::sin(radLat);
    const double cosLat = std::cos(radLat);
    const double N = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
    const double x = (N + h) * cosLat * std::cos(radLon);
    const double y = (N + h) * cosLat * std::sin(radLon);
    const double z = (N * (1.0 - e2) + h) * sinLat;
    return { x, y, z };
}

Sphere obbToSphere(const std::vector<double>& boxSpec) {
    if (boxSpec.size() < 12) return { {0, 0, 0}, 0 };
    double cx = boxSpec[0], cy = boxSpec[1], cz = boxSpec[2];
    double h1[3] = { boxSpec[3], boxSpec[4], boxSpec[5] };
    double h2[3] = { boxSpec[6], boxSpec[7], boxSpec[8] };
    double h3[3] = { boxSpec[9], boxSpec[10], boxSpec[11] };

    std::vector<Vector3> corners;
    corners.reserve(8);
    for (int i = 0; i < 8; i++) {
        double s1 = (i & 1) ? 1.0 : -1.0;
        double s2 = (i & 2) ? 1.0 : -1.0;
        double s3 = (i & 4) ? 1.0 : -1.0;
        corners.push_back({
            cx + s1 * h1[0] + s2 * h2[0] + s3 * h3[0],
            cy + s1 * h1[1] + s2 * h2[1] + s3 * h3[1],
            cz + s1 * h1[2] + s2 * h2[2] + s3 * h3[2]
        });
    }

    double minX = corners[0].x, maxX = corners[0].x;
    double minY = corners[0].y, maxY = corners[0].y;
    double minZ = corners[0].z, maxZ = corners[0].z;

    for (const auto& c : corners) {
        if (c.x < minX) minX = c.x;
        if (c.x > maxX) maxX = c.x;
        if (c.y < minY) minY = c.y;
        if (c.y > maxY) maxY = c.y;
        if (c.z < minZ) minZ = c.z;
        if (c.z > maxZ) maxZ = c.z;
    }

    double midX = 0.5 * (minX + maxX);
    double midY = 0.5 * (minY + maxY);
    double midZ = 0.5 * (minZ + maxZ);
    double dx = maxX - minX;
    double dy = maxY - minY;
    double dz = maxZ - minZ;
    double radius = 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz);

    return { {midX, midY, midZ}, radius };
}

// --- Helpers ---

// Extract a "session=" query parameter from a URL and update the session string.
static void adoptSessionFromUrl(const std::string& url, std::string& session) {
    std::size_t pos = url.find("session=");
    if (pos == std::string::npos) return;
    pos += 8; // length of "session="
    std::size_t end = url.find_first_of("&#", pos);
    if (end == std::string::npos) end = url.size();
    if (end > pos) {
        session = url.substr(pos, end - pos);
    }
}

// --- HTTP Helper ---

TileDownloader::TileDownloader(const std::string &apiKey, const std::string &cacheDir) :
		apiKey(apiKey), cacheDir{cacheDir}
{
}

#include "debug_log.h"

// CURL write callback function
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::vector<unsigned char>* buffer) {
    size_t totalSize = size * nmemb;
    buffer->insert(buffer->end(), (unsigned char*)contents, (unsigned char*)contents + totalSize);
    return totalSize;
}

// ...

std::pair<std::vector<unsigned char>, std::string>
TileDownloader::fetchUrlNonCached(const std::string& url) {
    std::vector<unsigned char> buffer;
    std::string contentType;
    log_debug("[Downloader] Fetching URL: " + url);

#ifdef _WIN32
    // Parse URL
    std::wstring wUrl(url.begin(), url.end());
    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    urlComp.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.length(), 0, &urlComp)) {
        std::cerr << "WinHttpCrackUrl failed" << std::endl;
        return { buffer, contentType };
    }

    std::wstring hostName(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring urlPath(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    std::wstring extraInfo(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
    std::wstring fullPath = urlPath + extraInfo;

    HINTERNET hSession = WinHttpOpen(L"LuantiEarth/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (hSession) {
        HINTERNET hConnect = WinHttpConnect(hSession, hostName.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                                    L"GET",
                                                    fullPath.c_str(),
                                                    NULL,
                                                    WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    WINHTTP_FLAG_SECURE);
            if (hRequest) {
                if (WinHttpSendRequest(hRequest,
                                       WINHTTP_NO_ADDITIONAL_HEADERS,
                                       0,
                                       WINHTTP_NO_REQUEST_DATA,
                                       0,
                                       0,
                                       0)) {
                    if (WinHttpReceiveResponse(hRequest, NULL)) {
                        // Query Content-Type header
                        DWORD dwSize = 0;
                        BOOL result = WinHttpQueryHeaders(hRequest,
                                                          WINHTTP_QUERY_CONTENT_TYPE,
                                                          WINHTTP_HEADER_NAME_BY_INDEX,
                                                          NULL,
                                                          &dwSize,
                                                          WINHTTP_NO_HEADER_INDEX);
                        DWORD err = GetLastError();
                        if (err == ERROR_INSUFFICIENT_BUFFER && dwSize > 0) {
                            std::vector<wchar_t> headerBuffer(dwSize / sizeof(wchar_t) + 1);
                            if (WinHttpQueryHeaders(hRequest,
                                                    WINHTTP_QUERY_CONTENT_TYPE,
                                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                                    headerBuffer.data(),
                                                    &dwSize,
                                                    WINHTTP_NO_HEADER_INDEX)) {
                                std::wstring wContentType(headerBuffer.data());
                                contentType = std::string(wContentType.begin(), wContentType.end());
                                std::cout << "[DEBUG] Content-Type: " << contentType << std::endl;
                            } else {
                                std::cerr << "[DEBUG] Second WinHttpQueryHeaders failed: "
                                          << GetLastError() << std::endl;
                            }
                        } else {
                            std::cerr << "[DEBUG] First WinHttpQueryHeaders error: "
                                      << err << " (dwSize=" << dwSize << ")" << std::endl;
                        }

                        // Read response body
                        DWORD dwDownloaded = 0;
                        do {
                            dwSize = 0;
                            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                            if (dwSize == 0) break;

                            std::vector<char> tempBuffer(dwSize);
                            if (WinHttpReadData(hRequest, tempBuffer.data(), dwSize, &dwDownloaded)) {
                                buffer.insert(buffer.end(),
                                              tempBuffer.begin(),
                                              tempBuffer.begin() + dwDownloaded);
                            }
                        } while (dwSize > 0);
                    } else {
                        std::cerr << "WinHttpReceiveResponse failed" << std::endl;
                    }
                } else {
                    std::cerr << "WinHttpSendRequest failed: " << GetLastError() << std::endl;
                }
                WinHttpCloseHandle(hRequest);
            } else {
                std::cerr << "WinHttpOpenRequest failed" << std::endl;
            }
            WinHttpCloseHandle(hConnect);
        } else {
            std::cerr << "WinHttpConnect failed" << std::endl;
        }
        WinHttpCloseHandle(hSession);
    } else {
        std::cerr << "WinHttpOpen failed" << std::endl;
    }
#else
    // CURL implementation for non-Windows
    CURL* curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: "
                      << curl_easy_strerror(res) << std::endl;
        } else {
            char* ct = nullptr;
            if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) {
                contentType = ct;
            }
        }
        curl_easy_cleanup(curl);
    }
#endif

    std::cout << "Downloaded " << buffer.size()
              << " bytes, Content-Type: " << contentType << std::endl;
    return { buffer, contentType };
}

std::vector<unsigned char> TileDownloader::fetchUrlPublic(const std::string& url) {
    return fetchUrl(url).first; // Return just the data, not content-type
}

// --- Traversal Logic ---

void parseNode(const json& node,
               const Sphere& regionSphere,
               const std::string& baseURL,
               std::string& session,
               const std::string& apiKey,
               std::vector<std::string>& glbUrls,
               TileDownloader* downloader) {

    static int nodeCount = 0;
    nodeCount++;
    if (nodeCount % 10 == 0) {
        std::cout << "Processed " << nodeCount
                  << " nodes, found " << glbUrls.size()
                  << " GLBs so far" << std::endl;
    }

    bool intersects = false;
    if (node.contains("boundingVolume") &&
        node["boundingVolume"].contains("box")) {
        std::vector<double> box = node["boundingVolume"]["box"].get<std::vector<double>>();
        Sphere sphere = obbToSphere(box);
        if (regionSphere.intersects(sphere)) intersects = true;
    } else {
        intersects = true;
    }

    if (!intersects) {
        if (nodeCount % 50 == 0) {
            std::cout << "  -> Node rejected by bounding sphere test" << std::endl;
        }
        return;
    }

    if (node.contains("children") && node["children"].is_array()) {
        for (const auto& child : node["children"]) {
            parseNode(child, regionSphere, baseURL, session, apiKey, glbUrls, downloader);
        }
        return;
    }

    // Leaf or content
    std::vector<json> contents;
    if (node.contains("content"))
        contents.push_back(node["content"]);
    if (node.contains("contents") && node["contents"].is_array()) {
        for (const auto& c : node["contents"]) contents.push_back(c);
    }

    for (const auto& content : contents) {
        if (!content.contains("uri")) continue;
        std::string uri = content["uri"].get<std::string>();
        std::cout << "Processing URI: " << uri << std::endl;

        // Construct full URL
        std::string fullUrl;
        if (uri.rfind("http", 0) == 0) {
            // Absolute HTTP URL
            fullUrl = uri;
        } else if (!uri.empty() && uri[0] == '/') {
            // Absolute path - need to extract scheme + host from baseURL
            size_t schemeEnd = baseURL.find("://");
            if (schemeEnd != std::string::npos) {
                size_t hostEnd = baseURL.find("/", schemeEnd + 3);
                if (hostEnd != std::string::npos) {
                    fullUrl = baseURL.substr(0, hostEnd) + uri;
                } else {
                    fullUrl = baseURL + uri;
                }
            } else {
                fullUrl = uri;
            }
        } else {
            // Relative path
            size_t lastSlash = baseURL.find_last_of("/");
            if (lastSlash != std::string::npos) {
                fullUrl = baseURL.substr(0, lastSlash + 1) + uri;
            } else {
                fullUrl = uri;
            }
        }

        // If the URI already has a session parameter, adopt it
        adoptSessionFromUrl(fullUrl, session);

        // Add params
        std::string separator = (fullUrl.find('?') == std::string::npos) ? "?" : "&";
        if (fullUrl.find("key=") == std::string::npos) {
            fullUrl += separator + std::string("key=") + apiKey;
            separator = "&";
        } else {
            separator = "&";
        }
        if (!session.empty() && fullUrl.find("session=") == std::string::npos) {
            fullUrl += separator + std::string("session=") + session;
        }

        std::cout << "Full URL: " << fullUrl << std::endl;

        if (fullUrl.find(".glb") != std::string::npos) {
            std::cout << "  -> Found GLB!" << std::endl;
            glbUrls.push_back(fullUrl);
        } else if (fullUrl.find(".json") != std::string::npos) {
            std::cout << "  -> Found JSON, recursing..." << std::endl;
            auto data = downloader->fetchUrlPublic(fullUrl);
            if (!data.empty()) {
                try {
                    json subJson = json::parse(data.begin(), data.end());
                    if (subJson.contains("root")) {
                        parseNode(subJson["root"], regionSphere, fullUrl,
                                  session, apiKey, glbUrls, downloader);
                    } else if (!subJson.empty()) {
                        // No "root" key - maybe it's directly a tileset node?
                        std::cout << "  -> JSON has no 'root', keys are: ";
                        for (auto it = subJson.begin(); it != subJson.end(); ++it) {
                            std::cout << it.key() << " ";
                        }
                        std::cout << std::endl;

                        // Try treating it as a tileset node directly
                        parseNode(subJson, regionSphere, fullUrl,
                                  session, apiKey, glbUrls, downloader);
                    } else {
                        std::cout << "  -> Empty JSON, treating as GLB" << std::endl;
                        glbUrls.push_back(fullUrl);
                    }
                } catch (...) {
                    // If it's not valid JSON, treat it as a GLB
                    std::cout << "  -> JSON parse failed, treating as GLB" << std::endl;
                    glbUrls.push_back(fullUrl);
                }
            }
        } else {
            // No extension - fetch and try to parse as JSON; fallback to GLB
            std::cout << "  -> No extension, fetching to check type..." << std::endl;
            auto data = downloader->fetchUrlPublic(fullUrl);
            if (!data.empty()) {
                try {
                    json subJson = json::parse(data.begin(), data.end());
                    if (subJson.contains("root")) {
                        std::cout << "    -> It's JSON, recursing..." << std::endl;
                        parseNode(subJson["root"], regionSphere, fullUrl,
                                  session, apiKey, glbUrls, downloader);
                    } else if (!subJson.empty()) {
                        std::cout << "    -> JSON has no 'root', recursing as node..." << std::endl;
                        parseNode(subJson, regionSphere, fullUrl,
                                  session, apiKey, glbUrls, downloader);
                    } else {
                        std::cout << "    -> Empty JSON, treating as GLB" << std::endl;
                        glbUrls.push_back(fullUrl);
                    }
                } catch (...) {
                    std::cout << "    -> JSON parse failed, treating as GLB" << std::endl;
                    glbUrls.push_back(fullUrl);
                }
            }
        }
    }
}

std::pair<std::vector<unsigned char>, std::string> TileDownloader::fetchUrl(
		const std::string &url)
{
	// Thread‑safe cache access
	std::string cacheFile;
	std::string typeFile;
	if (!cacheDir.empty()) {
		std::lock_guard<std::mutex> lock(cacheMutex);

		// Ensure the cache directory exists
		std::filesystem::create_directories(cacheDir);

		// Create a deterministic cache filename from the URL (hash based)
		// Strip key and session parameters from URL for caching
		std::string cacheUrl = url;
		auto stripParam = [&](const std::string& name) {
			std::string::size_type pos = cacheUrl.find(name + "=");
			while (pos != std::string::npos) {
				std::string::size_type end = cacheUrl.find('&', pos);
				if (end == std::string::npos) {
					cacheUrl.erase(pos);
				} else {
					cacheUrl.erase(pos, end - pos + 1);
				}
				if (!cacheUrl.empty() && (cacheUrl.back() == '?' || cacheUrl.back() == '&')) {
					cacheUrl.pop_back();
				}
				pos = cacheUrl.find(name + "=");
			}
		};
		stripParam("key");
		stripParam("session");
		std::hash<std::string> hasher;
		size_t hashValue = hasher(cacheUrl);
		cacheFile = cacheDir + "/" + std::to_string(hashValue) + ".bin";
		typeFile = cacheDir + "/" + std::to_string(hashValue) + ".type";

		// Try to load from cache
		if (std::filesystem::exists(cacheFile)) {
			std::vector<unsigned char> data;
			std::ifstream in(cacheFile, std::ios::binary);
			if (in) {
				in.unsetf(std::ios::skipws);
				std::streampos fileSize;
				in.seekg(0, std::ios::end);
				fileSize = in.tellg();
				in.seekg(0, std::ios::beg);
				data.reserve(static_cast<size_t>(fileSize));
				data.insert(data.begin(), std::istream_iterator<unsigned char>(in),
						std::istream_iterator<unsigned char>());
			}
			std::string contentType;
			if (std::filesystem::exists(typeFile)) {
				std::ifstream ct(typeFile);
				std::getline(ct, contentType);
			}
			return {data, contentType};
		}
	}
	// Not cached – fetch from network
	auto [data, contentType] = fetchUrlNonCached(url);

	// Store result in cache for future calls
	if (!data.empty() && !cacheFile.empty()) {
		std::ofstream out(cacheFile, std::ios::binary);
		out.write(reinterpret_cast<const char *>(data.data()), data.size());

		std::ofstream ct(typeFile);
		ct << contentType;
	}

	return {data, contentType};
}

std::vector<TileData> TileDownloader::downloadTiles(double lat,
                                                    double lon,
                                                    double elevation,
                                                    double radius) {
    std::vector<TileData> results;

    // 1. Get Elevation (skip for now)

    // 2. Compute search sphere
    Vector3 center = cartesianFromDegrees(lon, lat, elevation);
    Sphere regionSphere = { center, radius };

    // 3. Traverse
    std::string rootUrl = "https://tile.googleapis.com/v1/3dtiles/root.json?key=" + apiKey;
    std::string session;
    std::vector<std::string> glbUrls;

    auto [rootBytes, rootContentType] = fetchUrl(rootUrl);
    if (rootBytes.empty()) return results;

    try {
        json rootJson = json::parse(rootBytes.begin(), rootBytes.end());

        // Extract session if present
        if (rootJson.contains("session")) {
            session = rootJson["session"].get<std::string>();
            std::cout << "Extracted session from JSON: " << session << std::endl;
        } else {
            // Fallback: adopt from URL if it ever appears there
            adoptSessionFromUrl(rootUrl, session);
        }

        if (rootJson.contains("root")) {
            parseNode(rootJson["root"], regionSphere, rootUrl,
                      session, apiKey, glbUrls, this);
        }

    } catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
    }

    // 4. Download GLBs
    std::cout << "Found " << glbUrls.size() << " GLB URLs" << std::endl;
    for (const auto& url : glbUrls) {
        auto [data, contentType] = fetchUrl(url);
        if (!data.empty()) {
            results.push_back(TileData{ url, data });
        }
    }

    return results;
}
