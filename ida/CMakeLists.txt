cmake_minimum_required(VERSION 3.10)

project(LowLatencyApp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_CXX_FLAGS "-std=c++2a -Wall -Wextra -Werror -Wpedantic")
set(CMAKE_VERBOSE_MAKEFILE on)

# Find required packages for Binance integration
find_package(Boost 1.70.0 REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.9.0 QUIET)

if(NOT nlohmann_json_FOUND)
  # If nlohmann_json not found through find_package, use the system include path
  include_directories(/home/praveen/miniforge3/envs/om/include)
endif()

add_subdirectory(common)
add_subdirectory(exchange)
add_subdirectory(trading)
add_subdirectory(benchmarks)
add_subdirectory(testing)

list(APPEND LIBS libexchange)
list(APPEND LIBS libtrading)
list(APPEND LIBS libcommon)
list(APPEND LIBS pthread)
list(APPEND LIBS ${Boost_LIBRARIES})
list(APPEND LIBS ${OPENSSL_LIBRARIES})

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/exchange)
include_directories(${PROJECT_SOURCE_DIR}/trading)
include_directories(${Boost_INCLUDE_DIRS})
include_directories(${OPENSSL_INCLUDE_DIR})

add_executable(exchange_main exchange/exchange_main.cpp)
target_link_libraries(exchange_main PUBLIC ${LIBS})

add_executable(trading_main trading/trading_main.cpp)
target_link_libraries(trading_main PUBLIC ${LIBS})

add_executable(test_binance trading/test_binance.cpp)
target_link_libraries(test_binance PUBLIC ${LIBS})

add_executable(logger_benchmark benchmarks/logger_benchmark.cpp)
target_link_libraries(logger_benchmark PUBLIC ${LIBS})

add_executable(release_benchmark benchmarks/release_benchmark.cpp)
target_link_libraries(release_benchmark PUBLIC ${LIBS})

add_executable(hash_benchmark benchmarks/hash_benchmark.cpp)
target_link_libraries(hash_benchmark PUBLIC ${LIBS})
