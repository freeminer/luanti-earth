cmake_minimum_required(VERSION 3.14)
project(earth_native)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies
include(FetchContent)

# Lua (using LuaJIT compatible 5.1 headers usually, or just Lua 5.1)
# For simplicity, we'll try to find Lua on the system or fetch a compatible version.
# Since we are building a DLL for Luanti, we generally don't link against Lua statically 
# if Luanti exports symbols, but on Windows we usually need the import lib.
# Let's try to fetch a simple Lua library.

# We might not need to build Lua if we just need headers, but let's see.
# Actually, for a module, we need headers.

# cURL - Removed in favor of WinHTTP on Windows
# find_package(CURL REQUIRED)

# TinyGLTF (Header only, but depends on stb_image and json)
FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG        v2.9.7
)
FetchContent_MakeAvailable(tinygltf)

# Draco
FetchContent_Declare(
  draco
  GIT_REPOSITORY https://github.com/google/draco.git
  GIT_TAG        1.5.7
)
FetchContent_MakeAvailable(draco)

# nlohmann_json
if (NOT HAVE_NLOHMANN_JSON)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
)
FetchContent_MakeAvailable(json)
endif()

# Source files
file(GLOB SOURCES "src/*.cpp")

add_library(earth_native SHARED ${SOURCES})

# Include directories
target_include_directories(earth_native PRIVATE 
    src 
    ${tinygltf_SOURCE_DIR}
    ${draco_SOURCE_DIR}/src      # <-- add this line
    ${CMAKE_BINARY_DIR}          # <-- where draco/draco_features.h is generated
)

# Link libraries
# Link libraries
target_link_libraries(earth_native PRIVATE 
    draco::draco
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(earth_native PRIVATE winhttp)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(earth_native PRIVATE CURL::libcurl)
endif()



# No Lua dependencies needed for FFI-only DLL!
# We export standard C functions.

