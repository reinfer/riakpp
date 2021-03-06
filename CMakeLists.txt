project(riakpp)
cmake_minimum_required (VERSION 2.8)
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake_modules")

# Configuration Options
# ==============================================================================
OPTION(BUILD_EXAMPLES OFF "Enable building of the examples")
OPTION(BUILD_TESTS OFF "Enable building of tests")

# C++ Flags
# ==============================================================================

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Werror -g -pthread")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -std=c++11 -Werror -g -pthread")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Intel")
  # using Intel C++
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
  # using Visual Studio C++
endif()

# Package Dependencies
# ==============================================================================
find_package(
  Boost 1.53
    COMPONENTS
      system
    REQUIRED
)

find_package(ProtobufPlugin REQUIRED)

include_directories(
  "${Boost_INCLUDE_DIR}"
  "${PROTOBUF_INCLUDE_DIR}"
  "${PROJECT_BINARY_DIR}/src"
)

# Subdirectories
# ==============================================================================
include_directories("src" "include")

add_subdirectory("src")
add_subdirectory("examples")
add_subdirectory("test")

# Install Scripts
# ==============================================================================

file(GLOB RIAKPP_PUBLIC_HEADERS include/*.hpp)
install(FILES ${RIAKPP_PUBLIC_HEADERS} DESTINATION include/riakpp)
install(
  FILES
    ${PROJECT_BINARY_DIR}/src/riak.pb.h
    ${PROJECT_BINARY_DIR}/src/riak_kv.pb.h
  DESTINATION
    include/riakpp)
