# asuras_wrath - ReXGlue Recompiled Project
#
# The SDK package provides rexglue_project and the rexglue_setup_* functions.

cmake_minimum_required(VERSION 3.25)
project(asuras_wrath LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (REXSDK_DIR)
  add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
else ()
  find_package(rexglue 0.11.0 REQUIRED CONFIG)
endif ()
rexglue_project(asuras_wrath OUTPUT_DIR generated/default)

# Host sources. src/main.cpp stays on the executable.
set(ASURASWRATH_HOST_SOURCES
  src/asuras_wrath_hooks.cpp
  host/xam_workspace.cpp
  host/offline_network.cpp
)

# OBJECT and never STATIC, an archive drops the REX_EXPORT symbols.
add_library(asuras_wrath_common OBJECT ${ASURASWRATH_HOST_SOURCES})
rexglue_setup_host_library(asuras_wrath_common)

if (WIN32)
  enable_language(RC)
  add_executable(asuras_wrath WIN32 src/main.cpp src/wrathfall_icon.rc)
  target_link_libraries(asuras_wrath PRIVATE SDL3::SDL3)
  set_property(SOURCE src/wrathfall_icon.rc PROPERTY
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/host/launcher")
  set_property(SOURCE src/wrathfall_icon.rc PROPERTY
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/launcher/Launcher.ico")
else ()
  add_executable(asuras_wrath src/main.cpp)
endif ()

rexglue_setup_target(asuras_wrath)
target_link_libraries(asuras_wrath PRIVATE asuras_wrath_common)


# Exercise this project's generated loads/stores against the guest memory map.
add_executable(guest_layout_check EXCLUDE_FROM_ALL accuracy/guest_layout.cpp)
add_dependencies(guest_layout_check asuras_wrath_codegen)
target_include_directories(guest_layout_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(guest_layout_check PRIVATE rex::sdk)

add_subdirectory(host/console_ui)

if (WIN32)
  # Player packages include the compiled launcher and its game icon.
  if(CMAKE_HOST_WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/host/launcher/CMakeLists.txt")
    add_subdirectory(host/launcher)
  endif()

  add_executable(asura_gameplay_collector host/gameplay_collector.cpp)
  target_compile_definitions(asura_gameplay_collector PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(asura_gameplay_collector PRIVATE d3d11 dxgi windowsapp windowscodecs ole32 user32)
  add_dependencies(asuras_runtime asura_gameplay_collector)
endif()
