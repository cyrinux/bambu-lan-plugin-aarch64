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
target_include_directories(bbl_common PUBLIC src third_party/nlohmann/.. third_party/bambu)
target_link_libraries(bbl_common PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_library(bbl_net_core STATIC
  src/networking/mqtt_client.cpp
  src/networking/ssdp_listener.cpp
  src/networking/ftps_upload.cpp
  src/networking/printer_models.cpp
  src/networking/agent.cpp
  src/networking/detect.cpp
  src/networking/print_job.cpp)
target_link_libraries(bbl_net_core PUBLIC bbl_common CURL::libcurl)

add_library(bbl_source_core STATIC src/source/bambu_url.cpp src/source/camera_stream.cpp)
target_link_libraries(bbl_source_core PUBLIC bbl_common)

# Plugins, named exactly as the slicer dlopen()s them.
add_library(bambu_networking SHARED
  src/networking/exports_core.cpp
  src/networking/exports_cloud_stubs.cpp
  src/networking/exports_ft.cpp)
target_compile_definitions(bambu_networking PRIVATE BBL_LAN_PLUGIN_GIT="${BBL_GIT}")
target_link_libraries(bambu_networking PRIVATE bbl_net_core)

add_library(BambuSource SHARED src/source/exports.cpp)
target_link_libraries(BambuSource PRIVATE bbl_source_core)

foreach(lib bambu_networking BambuSource)
  target_link_options(${lib} PRIVATE -Wl,--no-undefined -Wl,--as-needed -static-libgcc)
endforeach()

if(BBL_LAN_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE bbl_net_core bbl_source_core)
  add_test(NAME unit_tests COMMAND unit_tests)

  # Manual tools for testing against a real printer.
  add_executable(bbl_lan_probe tests/probe.cpp)
  target_include_directories(bbl_lan_probe PRIVATE third_party/bambu third_party)
  target_link_libraries(bbl_lan_probe PRIVATE ${CMAKE_DL_LIBS})
endif()
