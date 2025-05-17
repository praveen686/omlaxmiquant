cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_CXX_FLAGS "-std=c++2a -Wall -Wextra -Werror -Wpedantic")
set(CMAKE_VERBOSE_MAKEFILE on)

# Find required packages for Binance tests
find_package(Boost 1.70.0 REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.9.0 QUIET)

if(NOT nlohmann_json_FOUND)
  # If nlohmann_json not found through find_package, use FetchContent
  include(FetchContent)
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

# Include the main project directories
include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/trading)
include_directories(${Boost_INCLUDE_DIRS})
include_directories(${OPENSSL_INCLUDE_DIR})

# Add the binance tests directory
add_subdirectory(binance)