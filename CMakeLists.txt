# C8051_dap_server — CMakeLists.txt
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KMI Music, Inc.
# Author: Eric Bateman <eric@musekinetics.com>
#
# Builds dap_server.exe: a Windows DAP server that drives SiC8051F.dll (AGDI)
# to enable VSCode debugging of Silicon Laboratories C8051F-series MCUs.
# Requires MSVC x86 (Win32) — the vendor DLL is 32-bit only.

cmake_minimum_required(VERSION 3.20)

project(C8051_dap_server LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This project targets Windows only (SiC8051F.dll is a 32-bit Windows DLL).")
endif()

if(NOT (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4))
  message(FATAL_ERROR
    "This project requires MSVC targeting x86 (Win32). "
    "Reconfigure with a 32-bit generator, e.g.: cmake -A Win32 ..")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Vendor DLLs — expected in silabs_ref/debug_dll/ (gitignored, not shipped)
# SiC8051F.dll  — Silicon Labs AGDI debug DLL (from Keil µVision install)
# USBHID.dll    — USB HID transport dependency of SiC8051F.dll
# ---------------------------------------------------------------------------
set(SILABS_DEBUG_DLL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/silabs_ref/debug_dll")
set(SILABS_SIC8051F_DLL  "${SILABS_DEBUG_DLL_DIR}/SiC8051F.dll")
set(SILABS_USBHID_DLL    "${SILABS_DEBUG_DLL_DIR}/USBHID.dll")

# ---------------------------------------------------------------------------
# nlohmann/json — fetched automatically at configure time
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# dap_server — AGDI-to-DAP bridge for VSCode debugging of C8051F-series MCUs
# ---------------------------------------------------------------------------
add_executable(dap_server
  "${CMAKE_CURRENT_SOURCE_DIR}/dap_server/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dap_server/dap_server.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dap_server/agdi_loader.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dap_server/hex_loader.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dap_server/bp_manager.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dap_server/run_control.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dap_server/registers.cpp"
)

target_include_directories(dap_server PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dap_server"
)

target_link_libraries(dap_server PRIVATE
  ws2_32
  nlohmann_json::nlohmann_json
)

# Static CRT so the exe is self-contained alongside the vendor DLLs.
set_target_properties(dap_server PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dap_server/bin"
)

# ---------------------------------------------------------------------------
# Post-build: copy vendor DLLs and adapter config into the output directory
# ---------------------------------------------------------------------------
foreach(_dll "${SILABS_SIC8051F_DLL}" "${SILABS_USBHID_DLL}")
  get_filename_component(_dll_name "${_dll}" NAME)
  if(EXISTS "${_dll}")
    add_custom_command(TARGET dap_server POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_dll}"
              "$<TARGET_FILE_DIR:dap_server>/${_dll_name}"
      COMMENT "Copying ${_dll_name} to output directory"
      VERBATIM)
  else()
    message(WARNING
      "${_dll_name} not found at ${_dll}. "
      "Copy it manually to the output directory alongside dap_server.exe.")
  endif()
endforeach()

# SiC8051F.wsp — MonConf adapter config read by SiC8051F.dll at runtime
add_custom_command(TARGET dap_server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/dap_server/SiC8051F.wsp"
          "$<TARGET_FILE_DIR:dap_server>/SiC8051F.wsp"
  COMMENT "Copying SiC8051F.wsp to output directory"
  VERBATIM)
