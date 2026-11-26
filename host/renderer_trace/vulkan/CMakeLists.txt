cmake_minimum_required(VERSION 3.25)
project(wrathfall_vulkan LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(rexglue 0.11.0 EXACT REQUIRED CONFIG)

set(project_tools "" CACHE PATH "WrathFall pinned build tools")
set(vulkan_include "" CACHE PATH "Khronos Vulkan 1.4.313 headers")
set(vma_include "" CACHE PATH "Vulkan Memory Allocator include directory")
set(glslang_source "" CACHE PATH "Pinned glslang source directory")
set(sdk_source "${project_tools}/rexglue-sdk-0.11.0")
set(xenos "${sdk_source}/src/gpu/xenos")

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
  set(CMAKE_AR "${project_tools}/llvm-20.1.8/bin/llvm-lib.exe")
  set(CMAKE_CXX_CREATE_STATIC_LIBRARY "<CMAKE_AR> /NOLOGO /OUT:<TARGET> <OBJECTS>")
endif()

set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${glslang_source}" glslang EXCLUDE_FROM_ALL)

# Share the pinned SDK's driver source inventory while linking its installed host ABI.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${xenos}/CMakeLists.txt")
file(READ "${xenos}/CMakeLists.txt" inventory)
set(vulkan_sources)
foreach(group BASE SPIRV VULKAN HOST VULKAN_HOST)
  if(group STREQUAL "VULKAN_HOST")
    set(stem HOST_VULKAN)
  else()
    set(stem "${group}")
  endif()
  string(REGEX MATCH "set\\(REXGPU_XENOS_${stem}_SOURCES[ \r\n]+([^)]*)\\)" matched "${inventory}")
  if(NOT matched)
    message(FATAL_ERROR "Missing SDK source list ${stem}")
  endif()
  string(REGEX MATCHALL "[A-Za-z0-9_/]+\\.cpp" sources "${CMAKE_MATCH_1}")
  list(TRANSFORM sources PREPEND "${xenos}/")
  list(APPEND vulkan_sources ${sources})
endforeach()

include(VulkanHooks.cmake)
set(fidelityfx "${project_tools}/renderer-deps/fsr-2.4.0/FidelityFX-SDK-2.4.0/Kits/FidelityFX")
set(streamline "${project_tools}/renderer-deps/streamline-2.14.16")
add_library(asura_vulkan SHARED ${vulkan_sources} live_scene.cpp dlss_vulkan.cpp fsr_vulkan.cpp)
target_include_directories(asura_vulkan PRIVATE "${sdk_source}/src" "${sdk_source}"
  "${sdk_source}/thirdparty/renderdoc" "${project_tools}/renderer-deps/xxhash"
  "${vulkan_include}" "${vma_include}" "${glslang_source}" "${xenos}"
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/include")
target_include_directories(asura_vulkan SYSTEM PRIVATE
  "${fidelityfx}/api/include" "${fidelityfx}/upscalers/include" "${streamline}/include")
target_include_directories(asura_vulkan PRIVATE "${project_tools}/../host/renderer_trace")
target_compile_definitions(asura_vulkan PRIVATE
  REX_GPU_XENOS_STATIC XXH_INLINE_ALL REXGLUE_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(asura_vulkan PRIVATE rex::base rex::xenos glslang SPIRV dxgi)
rexglue_link_host_allocator(asura_vulkan)
set_target_properties(asura_vulkan PROPERTIES OUTPUT_NAME rexgpu-xenos-vulkan)
