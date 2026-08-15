cmake_minimum_required(VERSION 3.16)
project(convr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CONVR_BUILD_DRIVER "Build the SteamVR driver" ON)
option(CONVR_BUILD_COMPANION "Build the companion UI" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Where the built driver folder is staged. This mirrors the installed layout
# exactly, so installing is a plain directory copy on both platforms.
# ---------------------------------------------------------------------------
if(WIN32)
  set(CONVR_PLATFORM_DIR "win64")
else()
  set(CONVR_PLATFORM_DIR "linux64")
endif()
set(CONVR_STAGE_DIR "${CMAKE_BINARY_DIR}/convr")

# SteamVR only ever loads bin/win64 and bin/linux64. A 32-bit build would
# stage a DLL into win64 that vrserver then refuses to load, with nothing in
# the log to say why - so fail here instead, while the cause is still obvious.
# Visual Studio 2019 and older default their generator platform to Win32.
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "conVR must be built 64-bit; this toolchain is ${CMAKE_SIZEOF_VOID_P}-byte "
    "pointers.\nWith a Visual Studio generator, configure with -A x64.")
endif()

# ---------------------------------------------------------------------------
# OpenVR: the driver needs headers only. It never links openvr_api - SteamVR
# resolves everything through the driver context at load time. So instead of
# cloning the ~200 MB SDK we fetch the single header, pinned and checksummed.
# Point CONVR_OPENVR_INCLUDE_DIR at a local SDK's headers/ to build offline.
# ---------------------------------------------------------------------------
set(CONVR_OPENVR_VERSION "v2.5.1" CACHE STRING "OpenVR tag to fetch headers from")
set(CONVR_OPENVR_INCLUDE_DIR "" CACHE PATH "Existing directory containing openvr_driver.h")

if(CONVR_OPENVR_INCLUDE_DIR STREQUAL "")
  set(_openvr_dir "${CMAKE_BINARY_DIR}/openvr_headers")
  set(_openvr_header "${_openvr_dir}/openvr_driver.h")
  if(NOT EXISTS "${_openvr_header}")
    message(STATUS "Fetching openvr_driver.h (${CONVR_OPENVR_VERSION})")
    file(DOWNLOAD
      "https://raw.githubusercontent.com/ValveSoftware/openvr/${CONVR_OPENVR_VERSION}/headers/openvr_driver.h"
      "${_openvr_header}"
      EXPECTED_HASH SHA256=de8e60d2870060ba88274572d8614b7eea1c9d7b3b3fbb68feb420499039c5fb
      TLS_VERIFY ON
      STATUS _openvr_status)
    list(GET _openvr_status 0 _openvr_code)
    if(NOT _openvr_code EQUAL 0)
      list(GET _openvr_status 1 _openvr_message)
      message(FATAL_ERROR
        "Could not download openvr_driver.h (${_openvr_message}).\n"
        "Download it manually and configure with "
        "-DCONVR_OPENVR_INCLUDE_DIR=/path/to/openvr/headers")
    endif()
  endif()
  set(CONVR_OPENVR_INCLUDE_DIR "${_openvr_dir}")
endif()

if(NOT EXISTS "${CONVR_OPENVR_INCLUDE_DIR}/openvr_driver.h")
  message(FATAL_ERROR "openvr_driver.h not found in ${CONVR_OPENVR_INCLUDE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Shared IPC shim, linked into both the driver and the companion.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(convr_common STATIC common/convr_ipc.cpp)
target_include_directories(convr_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/common")
target_link_libraries(convr_common PUBLIC Threads::Threads)
if(MSVC)
  # getenv() is perfectly correct here; MSVC deprecates it regardless.
  target_compile_definitions(convr_common PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

option(CONVR_BUILD_TESTS "Build the IPC self-test" OFF)
if(CONVR_BUILD_TESTS)
  add_executable(convr_ipc_selftest common/ipc_selftest.cpp)
  target_link_libraries(convr_ipc_selftest PRIVATE convr_common)
  set_target_properties(convr_ipc_selftest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  # Multi-config generators (Visual Studio) otherwise append Release/ or Debug/.
  foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(convr_ipc_selftest PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${_config} "${CMAKE_BINARY_DIR}/bin")
  endforeach()
  if(MSVC)
    target_compile_definitions(convr_ipc_selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  enable_testing()
  add_test(NAME ipc_roundtrip COMMAND convr_ipc_selftest)
endif()

if(CONVR_BUILD_DRIVER)
  add_subdirectory(driver)
endif()
if(CONVR_BUILD_COMPANION)
  add_subdirectory(companion)
endif()

# ---------------------------------------------------------------------------
# Install: drop the staged driver folder into the SteamVR drivers directory.
# Override with -DCONVR_STEAMVR_DRIVERS_DIR=... if Steam lives elsewhere.
# ---------------------------------------------------------------------------
if(WIN32)
  set(_default_drivers "C:/Program Files (x86)/Steam/steamapps/common/SteamVR/drivers")
else()
  set(_default_drivers "$ENV{HOME}/.steam/steam/steamapps/common/SteamVR/drivers")
endif()
set(CONVR_STEAMVR_DRIVERS_DIR "${_default_drivers}"
    CACHE PATH "SteamVR drivers directory the driver is installed into")

if(CONVR_BUILD_DRIVER)
  # Absolute DESTINATION on purpose: this ignores CMAKE_INSTALL_PREFIX and
  # always lands where SteamVR actually looks.
  install(DIRECTORY "${CONVR_STAGE_DIR}/"
          DESTINATION "${CONVR_STEAMVR_DRIVERS_DIR}/convr"
          COMPONENT driver
          USE_SOURCE_PERMISSIONS)
endif()

message(STATUS "conVR: driver staged in  ${CONVR_STAGE_DIR}")
message(STATUS "conVR: installs into     ${CONVR_STEAMVR_DRIVERS_DIR}/convr")
