# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.14)
project(namz VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NAMZ_BUILD_CLI   "Build the namz command-line tool" ON)
option(NAMZ_BUILD_TESTS "Build the namz round-trip tests"  ON)

# --- nlohmann/json (header-only, MIT) — the codec's only dependency -----------------------------------
include(FetchContent)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# --- namz: header-only interface library -------------------------------------------------------------
add_library(namz INTERFACE)
add_library(namz::namz ALIAS namz)
target_include_directories(namz INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(namz INTERFACE cxx_std_17)
target_link_libraries(namz INTERFACE nlohmann_json::nlohmann_json)

# --- namz_rig: the .namz device model (controls from metadata, selection policy) — std-only ------------
add_library(namz_rig INTERFACE)
add_library(namz::rig ALIAS namz_rig)
target_include_directories(namz_rig INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(namz_rig INTERFACE cxx_std_17)

# --- CLI ---------------------------------------------------------------------------------------------
if(NAMZ_BUILD_CLI)
  add_executable(namz_cli cli/namz.cpp)
  set_target_properties(namz_cli PROPERTIES OUTPUT_NAME namz)
  target_link_libraries(namz_cli PRIVATE namz::namz)
endif()

# --- tests -------------------------------------------------------------------------------------------
if(NAMZ_BUILD_TESTS)
  enable_testing()
  add_executable(namz_test tests/roundtrip.cpp)
  target_link_libraries(namz_test PRIVATE namz::namz)
  add_test(NAME namz_roundtrip COMMAND namz_test)

  add_executable(namz_test_adversarial tests/adversarial.cpp)
  target_link_libraries(namz_test_adversarial PRIVATE namz::namz)
  add_test(NAME namz_adversarial COMMAND namz_test_adversarial)

  add_executable(namz_test_rig tests/rig.cpp)
  target_link_libraries(namz_test_rig PRIVATE namz::rig namz::namz)   # namz::namz brings nlohmann for the rig.json loader
  add_test(NAME namz_rig COMMAND namz_test_rig)

  # The reference consumer of the cross-language conformance vectors (conformance/).
  add_executable(namz_test_conformance tests/conformance.cpp)
  target_link_libraries(namz_test_conformance PRIVATE namz::namz)
  target_compile_definitions(namz_test_conformance PRIVATE
    NAMZ_CONFORMANCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/conformance")
  add_test(NAME namz_conformance COMMAND namz_test_conformance)

  # The reference consumer of the RIG conformance fixtures (conformance/rig/) — the executable
  # .orbitrig contract between a capture tool (writer) and a player (reader).
  add_executable(namz_test_rig_conformance tests/rig_conformance.cpp)
  target_link_libraries(namz_test_rig_conformance PRIVATE namz::rig namz::namz)
  target_compile_definitions(namz_test_rig_conformance PRIVATE
    NAMZ_CONFORMANCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/conformance")
  add_test(NAME namz_rig_conformance COMMAND namz_test_rig_conformance)
endif()

# --- install (so downstreams can find_package or just copy the header) --------------------------------
include(GNUInstallDirs)
install(FILES include/namz.h include/namz_rig.h include/namz_rig_load.h include/namz_rig_write.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(NAMZ_BUILD_CLI)
  install(TARGETS namz_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
