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
  src/dsp/nam_slot.cpp
  src/control/midi.cpp
  src/control/midi_config.cpp
  src/control/control_socket.cpp
  src/control/midi_output.cpp
  src/control/midi_socket.cpp
  src/control/presets_config.cpp
)

target_include_directories(namzero-lite PRIVATE src third_party ${ALSA_INCLUDE_DIRS})
target_link_libraries(namzero-lite PRIVATE ${ALSA_LIBRARIES} Threads::Threads)

if(NAM_DIR)
  file(GLOB NAM_CORE_SOURCES "${NAM_DIR}/NAM/*.cpp" "${NAM_DIR}/NAM/*/*.cpp")
  add_library(nam_core STATIC ${NAM_CORE_SOURCES})
  target_include_directories(nam_core PUBLIC
    "${NAM_DIR}"
    "${NAM_DIR}/Dependencies/eigen"
    "${NAM_DIR}/Dependencies/nlohmann")
  target_compile_features(nam_core PUBLIC cxx_std_20)
  # Stessi flag del build ufficiale: con Eigen e le espressioni template,
  # -Ofast e LTO valgono molto piu' dei punti percentuali.
  # Percorso veloce dedicato ai modelli A2 (a2_fast.cpp): senza questa define
  # i modelli A2 passano dal WaveNet generico e costano ~6x di piu'.
  target_compile_definitions(nam_core PRIVATE NAM_ENABLE_A2_FAST)
  target_compile_options(nam_core PRIVATE -Ofast -flto=1)
  target_link_options(nam_core PRIVATE -flto=1)
  # I parser del core si registrano con inizializzatori statici: in una libreria
  # statica gli oggetti non referenziati non vengono estratti e il registro
  # resterebbe vuoto ("No config parser registered"). Serve il whole-archive.
  target_link_libraries(namzero-lite PRIVATE
    -Wl,--whole-archive nam_core -Wl,--no-whole-archive)
  target_link_options(namzero-lite PRIVATE -flto=1)
  target_compile_definitions(namzero-lite PRIVATE NAMZERO_HAVE_NAM=1)
  target_include_directories(namzero-lite PRIVATE "${NAM_DIR}")
  # Solo lo slot amp parla C++20 (Eigen + core NAM).
  set_source_files_properties(src/dsp/nam_slot.cpp PROPERTIES CXX_STANDARD 20)
  add_executable(bench_nam_slot test/bench_nam_slot.cpp src/dsp/nam_slot.cpp)
  target_include_directories(bench_nam_slot PRIVATE src "${NAM_DIR}")
  target_link_libraries(bench_nam_slot PRIVATE
    -Wl,--whole-archive nam_core -Wl,--no-whole-archive Threads::Threads)
  target_link_options(bench_nam_slot PRIVATE -flto=1)
  target_compile_definitions(bench_nam_slot PRIVATE NAMZERO_HAVE_NAM=1)
  target_compile_features(bench_nam_slot PRIVATE cxx_std_20)
  message(STATUS "NAM abilitato da ${NAM_DIR}")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(namzero-lite PRIVATE -O3)
endif()

# Test senza dipendenze audio: girano anche su un host senza ALSA.
enable_testing()
add_executable(wah_autoengage_test test/wah_autoengage_test.cpp)
target_include_directories(wah_autoengage_test PRIVATE src)
add_test(NAME wah_autoengage COMMAND wah_autoengage_test)

add_executable(drive_klon_test test/drive_klon_test.cpp)
target_include_directories(drive_klon_test PRIVATE src)
add_test(NAME drive_klon COMMAND drive_klon_test)

add_executable(drive_octave_test test/drive_octave_test.cpp)
target_include_directories(drive_octave_test PRIVATE src)
add_test(NAME drive_octave COMMAND drive_octave_test)

# Microbench manuale del costo dell'ottava del fuzz (si lancia sul Pi).
add_executable(octave_bench test/octave_bench.cpp)
target_include_directories(octave_bench PRIVATE src)

add_executable(switch_debounce_test test/switch_debounce_test.cpp)
target_include_directories(switch_debounce_test PRIVATE src)
add_test(NAME switch_debounce COMMAND switch_debounce_test)

add_executable(mod_fx_test test/mod_fx_test.cpp)
target_include_directories(mod_fx_test PRIVATE src)
add_test(NAME mod_fx COMMAND mod_fx_test)

add_executable(wah_tone_test test/wah_tone_test.cpp)
target_include_directories(wah_tone_test PRIVATE src)
add_test(NAME wah_tone COMMAND wah_tone_test)

add_executable(filter_test test/filter_test.cpp)
target_include_directories(filter_test PRIVATE src)
add_test(NAME filter COMMAND filter_test)

add_executable(pitch_shifter_test test/pitch_shifter_test.cpp)
target_include_directories(pitch_shifter_test PRIVATE src)
add_test(NAME pitch_shifter COMMAND pitch_shifter_test)

add_executable(eq_test test/eq_test.cpp src/dsp/chain.cpp)
target_include_directories(eq_test PRIVATE src)
add_test(NAME eq COMMAND eq_test)

add_executable(tuner_test test/tuner_test.cpp)
target_include_directories(tuner_test PRIVATE src)
add_test(NAME tuner COMMAND tuner_test)

# Benchmark manuale (non ctest): si misura sul Pi con ./build/tuner_bench.
add_executable(tuner_bench test/tuner_bench.cpp)
target_include_directories(tuner_bench PRIVATE src)

add_executable(midi_parser_test test/midi_parser_test.cpp)
target_include_directories(midi_parser_test PRIVATE src)
add_test(NAME midi_parser COMMAND midi_parser_test)

add_executable(midi_socket_test test/midi_socket_test.cpp src/control/midi_socket.cpp)
target_include_directories(midi_socket_test PRIVATE src)
add_test(NAME midi_socket COMMAND midi_socket_test)

add_executable(midi_config_test test/midi_config_test.cpp src/control/midi_config.cpp)
target_include_directories(midi_config_test PRIVATE src third_party)
add_test(NAME midi_config COMMAND midi_config_test)
