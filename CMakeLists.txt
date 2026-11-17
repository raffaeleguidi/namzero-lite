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

set(NAM_DIR "" CACHE PATH "Checkout di NeuralAmpModelerCore (abilita lo slot amp)")

add_executable(namzero-lite
  src/main.cpp
  src/audio/alsa_io.cpp
  src/audio/rt.cpp
  src/dsp/chain.cpp
  src/dsp/ir_slot.cpp
  src/dsp/partitioned_convolver.cpp
  src/dsp/wav.cpp
)

target_include_directories(namzero-lite PRIVATE src ${ALSA_INCLUDE_DIRS})
target_link_libraries(namzero-lite PRIVATE ${ALSA_LIBRARIES} Threads::Threads)

if(NAM_DIR)
  file(GLOB NAM_CORE_SOURCES "${NAM_DIR}/NAM/*.cpp")
  add_library(nam_core STATIC ${NAM_CORE_SOURCES})
  target_include_directories(nam_core PUBLIC
    "${NAM_DIR}"
    "${NAM_DIR}/Dependencies/eigen"
    "${NAM_DIR}/Dependencies/nlohmann")
  target_compile_features(nam_core PUBLIC cxx_std_20)
  target_compile_options(nam_core PRIVATE -O3)
  target_link_libraries(namzero-lite PRIVATE nam_core)
  target_compile_definitions(namzero-lite PRIVATE NAMZERO_HAVE_NAM=1)
  target_include_directories(namzero-lite PRIVATE "${NAM_DIR}")
  # Solo lo slot amp parla C++20 (Eigen + core NAM).
  set_source_files_properties(src/dsp/nam_slot.cpp PROPERTIES CXX_STANDARD 20)
  message(STATUS "NAM abilitato da ${NAM_DIR}")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(namzero-lite PRIVATE -O3)
endif()
