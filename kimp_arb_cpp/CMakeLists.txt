cmake_minimum_required(VERSION 3.20)
project(kimp_arb_cpp VERSION 1.0.0 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native")
endif()

# macOS/Homebrew paths
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/usr/local")
    set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3")
endif()

# Find packages
find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)

# Find other packages via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)
pkg_check_modules(FMT REQUIRED IMPORTED_TARGET fmt)
pkg_check_modules(YAML_CPP REQUIRED IMPORTED_TARGET yaml-cpp)

# Include directories
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/jwt-cpp/include
    ${Boost_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${SIMDJSON_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS}
    ${FMT_INCLUDE_DIRS}
    ${YAML_CPP_INCLUDE_DIRS}
)

# Collect source files
file(GLOB_RECURSE LIB_SOURCES
    "src/core/*.cpp"
    "src/memory/*.cpp"
    "src/network/*.cpp"
    "src/exchange/*.cpp"
    "src/data/*.cpp"
    "src/strategy/*.cpp"
    "src/execution/*.cpp"
    "src/utils/*.cpp"
)

# Create library for testing
add_library(kimp_lib STATIC ${LIB_SOURCES})
target_include_directories(kimp_lib PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/jwt-cpp/include
)
target_link_libraries(kimp_lib
    PUBLIC
        Boost::json
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
        PkgConfig::SIMDJSON
        PkgConfig::SPDLOG
        PkgConfig::FMT
        PkgConfig::YAML_CPP
)

# Main executable
add_executable(kimp_bot src/main.cpp)
target_link_libraries(kimp_bot PRIVATE kimp_lib)

# Install
install(TARGETS kimp_bot RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/kimp)

# Print configuration summary
message(STATUS "")
message(STATUS "=== KIMP Arbitrage Bot Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "=========================================")
message(STATUS "")
