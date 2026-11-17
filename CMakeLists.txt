cmake_minimum_required(VERSION 3.16)
project(namzero-lite CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

add_executable(namzero-lite
  src/main.cpp
  src/audio/alsa_io.cpp
  src/audio/rt.cpp
  src/dsp/chain.cpp
)

target_include_directories(namzero-lite PRIVATE src ${ALSA_INCLUDE_DIRS})
target_link_libraries(namzero-lite PRIVATE ${ALSA_LIBRARIES} Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(namzero-lite PRIVATE -O3)
endif()
