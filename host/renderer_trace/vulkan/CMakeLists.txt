cmake_minimum_required(VERSION 3.25)
project(wrathfall_vulkan LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(rexglue 0.11.0 EXACT REQUIRED CONFIG)

set(project_tools "" CACHE PATH "WrathFall pinned build tools")
set(vulkan_include "" CACHE PATH "Khronos Vulkan 1.4.313 headers")
set(vma_include "" CACHE PATH "Vulkan Memory Allocator include directory")
set(glslang_source "" CACHE PATH "Pinned glslang source directory")
set(dxc_executable "" CACHE FILEPATH "Optional DXC executable for the cross-compiled HLSL probes")
set(sdk_source "${project_tools}/rexglue-sdk-0.11.0")
set(xenos "${sdk_source}/src/gpu/xenos")

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
  set(CMAKE_AR "${project_tools}/llvm-20.1.8/bin/llvm-lib.exe")
  set(CMAKE_CXX_CREATE_STATIC_LIBRARY "<CMAKE_AR> /NOLOGO /OUT:<TARGET> <OBJECTS>")
endif()

set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${glslang_source}" glslang EXCLUDE_FROM_ALL)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
  get_target_property(glslang_link_options glslangValidator LINK_OPTIONS)
  list(REMOVE_ITEM glslang_link_options "-Wl,-undefined,error")
  set_property(TARGET glslangValidator PROPERTY LINK_OPTIONS "${glslang_link_options}")
endif()

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
set(temporal_shaders)
foreach(pass IN ITEMS Unpack Motion Compose History)
  string(TOUPPER "${pass}" define)
  set(shader "${CMAKE_CURRENT_BINARY_DIR}/include/temporal${pass}.h")
  add_custom_command(OUTPUT "${shader}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/include"
    COMMAND glslangValidator -V --target-env vulkan1.2 "-D${define}=1"
      --vn "kTemporal${pass}" -o "${shader}" "${CMAKE_CURRENT_SOURCE_DIR}/live_temporal.comp"
    DEPENDS glslangValidator "${CMAKE_CURRENT_SOURCE_DIR}/live_temporal.comp"
    VERBATIM)
  list(APPEND temporal_shaders "${shader}")
endforeach()
add_custom_target(asura_vulkan_shaders DEPENDS ${temporal_shaders})

set(fidelityfx "${project_tools}/renderer-deps/fsr-2.4.0/FidelityFX-SDK-2.4.0/Kits/FidelityFX")
set(streamline "${project_tools}/renderer-deps/streamline-2.14.16")
set(vulkan_upscalers dlss_vulkan.cpp fsr_vulkan.cpp)
set(vendor_include "${fidelityfx}/api/include" "${fidelityfx}/upscalers/include" "${streamline}/include")
add_library(asura_vulkan SHARED ${vulkan_sources} live_scene.cpp live_temporal.cpp
  ${vulkan_upscalers})
add_executable(asura_vulkan_probe EXCLUDE_FROM_ALL temporal_probe.cpp ${vulkan_upscalers})
set_target_properties(asura_vulkan_probe PROPERTIES OUTPUT_NAME temporal_probe)
target_compile_definitions(asura_vulkan_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

foreach(target IN ITEMS asura_vulkan asura_vulkan_probe)
  add_dependencies(${target} asura_vulkan_shaders)
endforeach()

target_include_directories(asura_vulkan PRIVATE "${sdk_source}/src" "${sdk_source}"
  "${sdk_source}/thirdparty/renderdoc" "${project_tools}/renderer-deps/xxhash"
  "${vulkan_include}" "${vma_include}" "${glslang_source}" "${xenos}"
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/include"
  "${project_tools}/../host/renderer_trace" "${project_tools}/../host")
target_include_directories(asura_vulkan SYSTEM PRIVATE ${vendor_include})
target_include_directories(asura_vulkan_probe PRIVATE "${vulkan_include}"
  "${CMAKE_CURRENT_BINARY_DIR}/include" "${project_tools}/../host/renderer_trace")
target_include_directories(asura_vulkan_probe SYSTEM PRIVATE ${vendor_include})
target_compile_definitions(asura_vulkan PRIVATE
  REX_GPU_XENOS_STATIC XXH_INLINE_ALL REXGLUE_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(asura_vulkan PRIVATE rex::base rex::xenos glslang SPIRV dxgi)
rexglue_link_host_allocator(asura_vulkan)
set_target_properties(asura_vulkan PROPERTIES OUTPUT_NAME rexgpu-xenos-vulkan)

add_custom_target(check_vulkan_motion
  COMMAND "$<TARGET_FILE:asura_vulkan_probe>" motion
  DEPENDS asura_vulkan_probe
  USES_TERMINAL
  VERBATIM)

if(dxc_executable)
  set(hlsl_outputs)
  foreach(shader IN ITEMS depth_motion fsr3_reactive)
    set(source "${project_tools}/../host/renderer_trace/${shader}.hlsl")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    set(listing "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spvasm")
    add_custom_command(OUTPUT "${output}" "${listing}"
      COMMAND "${dxc_executable}" -spirv -fspv-target-env=vulkan1.2 -fvk-use-dx-layout
        -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-u-shift 4 0 -fvk-s-shift 6 0
        -T cs_6_0 -E CSMain -Fo "${output}" -Fc "${listing}" "${source}"
      DEPENDS "${dxc_executable}" "${source}"
      VERBATIM)
    list(APPEND hlsl_outputs "${output}" "${listing}")
  endforeach()
  add_custom_target(vulkan_hlsl_spirv DEPENDS ${hlsl_outputs})
endif()
