cmake_minimum_required(VERSION 3.20)
project(bambu_lan_plugin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BBL_LAN_BUILD_TESTS "Build unit tests" ON)

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

execute_process(COMMAND git describe --always --dirty
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE BBL_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT BBL_GIT)
  set(BBL_GIT "nogit")
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Static helper libraries shared by the plugins and the tests.
add_library(bbl_common STATIC src/common/log.cpp src/common/tls_socket.cpp)
target_include_directories(bbl_common PUBLIC src third_party)
target_link_libraries(bbl_common PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

# -z nodelete: OrcaSlicer's plugin hot reload dlclose()s the library without
# destroy_agent and keeps using the old agent pointer; keeping the code mapped
# makes that safe (threads and callbacks stay valid).
set(BBL_PLUGIN_LINK_OPTIONS -Wl,--no-undefined -Wl,--as-needed -Wl,-z,nodelete -static-libgcc)

set(BBL_NET_CORE_SOURCES
  src/networking/mqtt_client.cpp
  src/networking/ssdp_listener.cpp
  src/networking/ftps_upload.cpp
  src/networking/printer_models.cpp
  src/networking/agent.cpp
  src/networking/detect.cpp
  src/networking/print_job.cpp)

# libbambu_networking is built once per slicer ABI: the exported functions take
# std::string / std::function / structs by value, so each slicer's own headers
# (third_party/<abi>/) must be used. OUTPUT_NAME is the file name before the
# installer renames it for the slicer (see scripts/install.sh).
#   target            ABI headers            output
#   bambu_networking  third_party/bambu      libbambu_networking.so       (Bambu Studio)
#   orca_networking   third_party/orca       libbambu_networking_orca.so  (OrcaSlicer)
function(bbl_networking_variant target abi_dir output_name defs)
  add_library(${target}_core STATIC ${BBL_NET_CORE_SOURCES})
  target_include_directories(${target}_core PUBLIC ${abi_dir})
  target_compile_definitions(${target}_core PUBLIC ${defs})
  target_link_libraries(${target}_core PUBLIC bbl_common CURL::libcurl)

  add_library(${target} SHARED
    src/networking/exports_core.cpp
    src/networking/exports_cloud_stubs.cpp
    src/networking/exports_ft.cpp)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name})
  target_compile_definitions(${target} PRIVATE BBL_LAN_PLUGIN_GIT="${BBL_GIT}")
  target_link_libraries(${target} PRIVATE ${target}_core)
  target_link_options(${target} PRIVATE ${BBL_PLUGIN_LINK_OPTIONS})
endfunction()

bbl_networking_variant(bambu_networking ${CMAKE_SOURCE_DIR}/third_party/bambu bambu_networking BBL_TARGET_STUDIO)
bbl_networking_variant(orca_networking ${CMAKE_SOURCE_DIR}/third_party/orca bambu_networking_orca BBL_TARGET_ORCA)

add_library(bbl_source_core STATIC src/source/bambu_url.cpp src/source/camera_stream.cpp)
target_link_libraries(bbl_source_core PUBLIC bbl_common)

# libBambuSource is shared by both slicers: Orca's BambuTunnel.h is a subset
# of Studio's with identical structs.
add_library(BambuSource SHARED src/source/exports.cpp)
target_include_directories(BambuSource PRIVATE third_party/bambu)
target_link_libraries(BambuSource PRIVATE bbl_source_core)
target_link_options(BambuSource PRIVATE ${BBL_PLUGIN_LINK_OPTIONS})

if(BBL_LAN_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE bambu_networking_core bbl_source_core)
  add_test(NAME unit_tests COMMAND unit_tests)

  # Manual tools for testing against a real printer.
  # Manual/integration tool, one per ABI (it calls the plugin through the
  # slicer's own function-pointer types).
  add_executable(bbl_lan_probe tests/probe.cpp)
  target_include_directories(bbl_lan_probe PRIVATE third_party/bambu third_party)
  target_link_libraries(bbl_lan_probe PRIVATE ${CMAKE_DL_LIBS})
  add_executable(bbl_lan_probe_orca tests/probe.cpp)
  target_include_directories(bbl_lan_probe_orca PRIVATE third_party/orca third_party/bambu third_party)
  target_compile_definitions(bbl_lan_probe_orca PRIVATE BBL_TARGET_ORCA)
  target_link_libraries(bbl_lan_probe_orca PRIVATE ${CMAKE_DL_LIBS})
endif()
