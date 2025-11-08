cmake_minimum_required(VERSION 3.15)
project(lethe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Place executables in '/build/bin'
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_FLAGS_DEBUG "-ggdb3 -O0 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -fsanitize=undefined -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -funroll-loops -flto")

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Allow #include "foo.h" instead of #include "src/foo.h"
# include_directories(${SRC})
# Sets libraries to be linked to all targets
# link_libraries(pthread)

# Configure with:
# cmake -DVOLIMEM_ROOT_DIR=/path/to/volimem ..
set(VOLIMEM_ROOT_DIR "$ENV{HOME}/desk/voli" CACHE PATH "Root directory of the VoliMem installation")
# Find VoliMem include directory and library
find_path(VOLIMEM_INCLUDE_DIR
          NAMES volimem/volimem.h 
          HINTS "${VOLIMEM_ROOT_DIR}/include"
          REQUIRED)
find_library(VOLIMEM_LIBRARY
             NAMES volimem # the library name (libvolimem.so)
             HINTS "${VOLIMEM_ROOT_DIR}/build/lib" 
             REQUIRED) 
if(VOLIMEM_INCLUDE_DIR AND VOLIMEM_LIBRARY)
  message(STATUS "Found VoliMem library and include directory:")
  message(STATUS "\tLibrary: ${VOLIMEM_LIBRARY}")
  message(STATUS "\tInclude: ${VOLIMEM_INCLUDE_DIR}")
else()
  message(FATAL_ERROR "VoliMem include directory or library not found. "
                      "Searched in ${VOLIMEM_ROOT_DIR}. "
                      "Ensure VoliMem is installed correctly or set VOLIMEM_ROOT_DIR.")
endif()

# Find RDMA libraries
find_library(IBVERBS_LIBRARY ibverbs HINTS "/usr/lib" REQUIRED)
find_library(RDMACM_LIBRARY rdmacm HINTS "/usr/lib" REQUIRED)
if(IBVERBS_LIBRARY AND RDMACM_LIBRARY)
  message(STATUS "Found RDMA libraries:")
  message(STATUS "\tibverbs: ${IBVERBS_LIBRARY}")
  message(STATUS "\trdmacm: ${RDMACM_LIBRARY}")
else()
  message(FATAL_ERROR "RDMA libraries not found. Ensure they are installed correctly.")
endif()

# Helper target that carries the shared link/include settings
add_library(common INTERFACE)
# Allow `#include "foo.h"` instead of `#include "src/foo.h"`
target_include_directories(common INTERFACE ${SRC})
# Add VoliMem as a system include
target_include_directories(common SYSTEM INTERFACE ${VOLIMEM_INCLUDE_DIR})
target_link_libraries(common INTERFACE pthread ${VOLIMEM_LIBRARY})

# Common source files
set(COMMON_SOURCES
  ${SRC}/utils.cpp
  ${SRC}/swapper.cpp
)

# Playground
# add_executable(playground
#   ${COMMON_SOURCES}
#   ${SRC}/playground.cpp
# )
# target_link_libraries(playground PRIVATE common)

# Server
add_executable(server
  ${COMMON_SOURCES}
  ${SRC}/rdma/common.cpp
  ${SRC}/rdma/server.cpp
)
target_link_libraries(server PRIVATE common ${IBVERBS_LIBRARY} ${RDMACM_LIBRARY})

# Client
add_executable(client
  ${COMMON_SOURCES}
  ${SRC}/rdma/common.cpp
  ${SRC}/rdma/client.cpp
  ${SRC}/storage/rdma_storage.cpp
)
target_link_libraries(client PRIVATE common ${IBVERBS_LIBRARY} ${RDMACM_LIBRARY})

# Shared library to inject the swapper into applications via LD_PRELOAD
add_library(lethe SHARED
  ${COMMON_SOURCES}
  ${SRC}/rdma/common.cpp
  ${SRC}/storage/rdma_storage.cpp
  ${SRC}/rdma/hook.cpp
)
# Link liblethe.so against all necessary libraries
target_link_libraries(lethe PRIVATE
  common
  ${IBVERBS_LIBRARY}
  ${RDMACM_LIBRARY}
)
# Place the shared library under '/build/lib'
set_target_properties(lethe PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# If no build type is specified (-DCMAKE_BUILD_TYPE=Release) default to Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose build type: Debug, Release, RelWithDebInfo, MinSizeRel" FORCE)
endif()
