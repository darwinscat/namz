# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.14)
project(namz VERSION 4.4.1 LANGUAGES CXX)

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
  target_compile_definitions(namz_test PRIVATE NAMZ_PROJECT_VERSION="${PROJECT_VERSION}")   # the header must state it
  add_test(NAME namz_roundtrip COMMAND namz_test)

  add_executable(namz_test_adversarial tests/adversarial.cpp)
  target_link_libraries(namz_test_adversarial PRIVATE namz::namz)
  add_test(NAME namz_adversarial COMMAND namz_test_adversarial)

  add_executable(namz_test_f16 tests/f16.cpp)
  target_link_libraries(namz_test_f16 PRIVATE namz::namz)
  add_test(NAME namz_f16 COMMAND namz_test_f16)

  # The CLI's float16: --f16 and the .nam16z name must agree, and what it writes reads back as float16.
  if(NAMZ_BUILD_CLI)
    set(_vec ${CMAKE_CURRENT_SOURCE_DIR}/conformance/vectors)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/cli-f16)
    file(MAKE_DIRECTORY ${_out})
    add_test(NAME namz_cli_f16_encode COMMAND namz_cli encode ${_vec}/nested.nam ${_out}/nested.nam16z --f16)
    set_tests_properties(namz_cli_f16_encode PROPERTIES FIXTURES_SETUP cli_f16)
    add_test(NAME namz_cli_f16_map COMMAND namz_cli map ${_out}/nested.nam16z)
    set_tests_properties(namz_cli_f16_map PROPERTIES FIXTURES_REQUIRED cli_f16
                         PASS_REGULAR_EXPRESSION "float16 weights")
    add_test(NAME namz_cli_f16_decode COMMAND namz_cli decode ${_out}/nested.nam16z ${_out}/nested.nam)
    set_tests_properties(namz_cli_f16_decode PROPERTIES FIXTURES_REQUIRED cli_f16)
    add_test(NAME namz_cli_f16_verify COMMAND namz_cli verify ${_vec}/nested.nam --f16)
    set_tests_properties(namz_cli_f16_verify PROPERTIES PASS_REGULAR_EXPRESSION "nam16z=[0-9]+ .*idempotent=yes")
    # A refusal is checked whole — the exit code, the message, and that no output file exists afterwards —
    # by tests/cli-expect.cmake, since PASS_REGULAR_EXPRESSION ignores the exit code and WILL_FAIL passes on
    # any failure at all.
    set(_expect ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:namz_cli>)
    set(_script -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli-expect.cmake --)
    add_test(NAME namz_cli_f16_needs_name COMMAND ${_expect} -DEXIT=2 "-DSTDERR=name the output \\.nam16z"
             -DOUT=${_out}/x.namz ${_script} encode ${_vec}/nested.nam ${_out}/x.namz --f16)
    add_test(NAME namz_cli_f16_needs_flag COMMAND ${_expect} -DEXIT=2 "-DSTDERR=pass --f16"
             -DOUT=${_out}/x.nam16z ${_script} encode ${_vec}/nested.nam ${_out}/x.nam16z)
    add_test(NAME namz_cli_f16_overflow COMMAND ${_expect} -DEXIT=1 "-DSTDERR=65520"   # FLT_MAX has no half
             -DOUT=${_out}/edge.nam16z ${_script} encode ${_vec}/edge-floats.nam ${_out}/edge.nam16z --f16)
    add_test(NAME namz_cli_verify_unknown_option COMMAND ${_expect} -DEXIT=2 "-DSTDERR=unknown option: --bogus"
             ${_script} verify ${_vec}/nested.nam --bogus)
    # decode and map refuse what they do not know, as encode and verify do — before anything is written
    add_test(NAME namz_cli_decode_unknown_option COMMAND ${_expect} -DEXIT=2 "-DSTDERR=unknown option: --bogus"
             -DOUT=${_out}/bogus.nam ${_script} decode ${_vec}/nested.namz ${_out}/bogus.nam --bogus)
    add_test(NAME namz_cli_map_unknown_option COMMAND ${_expect} -DEXIT=2 "-DSTDERR=unknown option: --bogus"
             ${_script} map ${_vec}/nested.namz --bogus)
    add_test(NAME namz_cli_map_json_unknown_option COMMAND ${_expect} -DEXIT=2 "-DSTDERR=unknown option: --bogus"
             ${_script} map ${_vec}/nested.namz --json --bogus)
    # map --json is JSON whatever a value holds: a tab and a newline come out escaped, never raw
    add_test(NAME namz_cli_map_json_encode COMMAND namz_cli encode ${_vec}/nested.nam ${_out}/escapes.namz
             --set "note=tab\there\nnewline")
    set_tests_properties(namz_cli_map_json_encode PROPERTIES FIXTURES_SETUP cli_map_json)
    add_test(NAME namz_cli_map_json_escapes COMMAND ${_expect} -DEXIT=0 "-DSTDERR=^$"
             -DSTDOUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/cli-map-json.expected
             ${_script} map ${_out}/escapes.namz --json)
    set_tests_properties(namz_cli_map_json_escapes PROPERTIES FIXTURES_REQUIRED cli_map_json)
  endif()

  # The documents name the release the code is: the README's FetchContent tag, the CHANGELOG's newest
  # entry, and a link for every entry.
  add_test(NAME namz_docs_version COMMAND ${CMAKE_COMMAND} -DVERSION=${PROJECT_VERSION}
           -DROOT=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/docs-version.cmake)

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
