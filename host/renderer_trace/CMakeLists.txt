cmake_minimum_required(VERSION 3.25)
project(asura_d3d12_trace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(rexglue 0.11.0 EXACT REQUIRED CONFIG)

set(project_tools "${CMAKE_CURRENT_SOURCE_DIR}/../../.tools" CACHE PATH "Pinned project tools")
set(sdk_source "${project_tools}/rexglue-sdk-0.11.0")
set(xxhash_include "${project_tools}/renderer-deps/xxhash")
set(xenos_source "${sdk_source}/src/gpu/xenos")
if(NOT EXISTS "${xenos_source}/d3d12/render_target_cache.cpp" OR
   NOT EXISTS "${xxhash_include}/xxhash.h")
  message(FATAL_ERROR "The renderer trace build needs the pinned ReXGlue source and xxHash header.")
endif()

set(d3d12_sources
  command_processor.cpp
  draw_extent_estimator.cpp
  draw_state.cpp
  index_conversion.cpp
  pipeline/render_target/cache.cpp
  pipeline/shader/interpreter.cpp
  pipeline/shader/shader.cpp
  pipeline/shader/translator.cpp
  pipeline/shader/translator_disasm.cpp
  pipeline/shader/dxbc.cpp
  pipeline/shader/dxbc_translator.cpp
  pipeline/shader/dxbc_translator_alu.cpp
  pipeline/shader/dxbc_translator_fetch.cpp
  pipeline/shader/dxbc_translator_memexport.cpp
  pipeline/shader/dxbc_translator_om.cpp
  pipeline/texture/cache.cpp
  primitive_processor.cpp
  sampler_info.cpp
  shared_memory.cpp
  xenos_graphics_driver_base.cpp
  host/fixed_point.cpp
  host/graphics_upload_buffer_pool.cpp
  host/imgui_overlay.cpp
  host/immediate_drawer.cpp
  host/presenter.cpp
  host/renderdoc_api.cpp
  host/surface.cpp
  host/xenos_presentation.cpp
  host/d3d12/d3d12_cpu_descriptor_pool.cpp
  host/d3d12/d3d12_descriptor_heap_pool.cpp
  host/d3d12/d3d12_immediate_drawer.cpp
  host/d3d12/d3d12_objects.cpp
  host/d3d12/d3d12_presenter.cpp
  host/d3d12/d3d12_provider.cpp
  host/d3d12/d3d12_submission_tracker.cpp
  host/d3d12/d3d12_upload_buffer_pool.cpp
  d3d12/command_processor.cpp
  d3d12/d3d12_xenos_graphics_driver.cpp
  d3d12/d3d12_xenos_provider.cpp
  d3d12/deferred_command_list.cpp
  d3d12/pipeline_cache.cpp
  d3d12/primitive_processor.cpp
  d3d12/shader.cpp
  d3d12/shared_memory.cpp
  d3d12/texture_cache.cpp
)
list(TRANSFORM d3d12_sources PREPEND "${xenos_source}/")

# Add a RenderDoc marker to Xenos resolves while keeping the pinned source intact.
set(resolve_source "${xenos_source}/d3d12/render_target_cache.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${resolve_source}")
file(READ "${resolve_source}" resolve_text)
set(anchor "  bool copied = false;\n  if (resolve_info.copy_dest_extent_length) {")
string(FIND "${resolve_text}" "${anchor}" anchor_position)
if(anchor_position EQUAL -1)
  message(FATAL_ERROR "The pinned renderer's resolve implementation changed.")
endif()
set(trace_code [=[
  if (resolve_info.copy_dest_extent_length) {
    char marker[192];
    std::snprintf(marker, sizeof(marker),
        "Xenos resolve frame=%llu %s EDRAM=%u dest=%08X range=%08X+%u rect=%ux%u format=%u",
        static_cast<unsigned long long>(command_processor_.GetCurrentFrame()),
        resolve_info.IsCopyingDepth() ? "depth" : "color",
        resolve_info.IsCopyingDepth() ? resolve_info.depth_original_base
                                      : resolve_info.color_original_base,
        resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length,
        resolve_info.coordinate_info.width_div_8 * 8u, resolve_info.height_div_8 * 8u,
        static_cast<unsigned>(resolve_info.copy_dest_info.copy_dest_format));
    command_processor_.GetDeferredCommandList().InsertDebugMarker(marker);
  }

]=])
string(REPLACE "${anchor}" "${trace_code}${anchor}" resolve_text "${resolve_text}")
string(REPLACE "#include \"../shaders/" "#include \"shaders/" resolve_text "${resolve_text}")
string(PREPEND resolve_text "#include <cstdio>\n")
set(traced_resolve "${CMAKE_CURRENT_BINARY_DIR}/render_target_cache.cpp")
file(CONFIGURE OUTPUT "${traced_resolve}" CONTENT "${resolve_text}" @ONLY)

add_library(asura_d3d12_trace SHARED
  ${d3d12_sources}
  "${traced_resolve}"
  "${sdk_source}/thirdparty/dxbc/DXBCChecksum.cpp"
)
target_include_directories(asura_d3d12_trace PRIVATE
  "${sdk_source}" "${sdk_source}/src"
  "${xenos_source}"
  "${sdk_source}/thirdparty/dxc/include"
  "${sdk_source}/thirdparty/renderdoc"
  "${xxhash_include}"
)
target_compile_definitions(asura_d3d12_trace PRIVATE
  REX_GPU_XENOS_STATIC XXH_INLINE_ALL
  REXGLUE_BUILD_CONFIG="$<CONFIG>"
)
target_link_libraries(asura_d3d12_trace PRIVATE rex::base rex::xenos d3d12 dxgi dxguid)
rexglue_link_host_allocator(asura_d3d12_trace)
set_target_properties(asura_d3d12_trace PROPERTIES OUTPUT_NAME rexgpu-xenos-d3d12)

option(ASURA_TEMPORAL_PROBE "Build the Direct3D 12 vendor upscaler smoke test" OFF)
option(ASURA_LIVE_DLSS_QA "Build the isolated in-game DLSS capture path" OFF)
if(ASURA_TEMPORAL_PROBE OR ASURA_LIVE_DLSS_QA)
  set(fidelityfx_root
    "${project_tools}/renderer-deps/fsr-2.4.0/FidelityFX-SDK-2.4.0/Kits/FidelityFX")
  set(streamline_include "${project_tools}/renderer-deps/streamline-2.14.16/include")
  if(NOT EXISTS "${fidelityfx_root}/upscalers/include/ffx_upscale.h" OR
     NOT EXISTS "${streamline_include}/sl_dlss.h")
    message(FATAL_ERROR "The temporal probe needs FSR SDK 2.4.0 and Streamline 2.14.16.")
  endif()
  find_program(ASURA_FXC fxc REQUIRED HINTS "$ENV{WindowsSdkVerBinPath}/x64")
  set(depth_motion_bytecode "${CMAKE_CURRENT_BINARY_DIR}/depth_motion_bytecode.h")
  add_custom_command(OUTPUT "${depth_motion_bytecode}"
    COMMAND "${ASURA_FXC}" /nologo /O3 /T cs_5_1 /E CSMain
      /Fh "${depth_motion_bytecode}" /Vn asura_depth_motion
      "${CMAKE_CURRENT_SOURCE_DIR}/depth_motion.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/depth_motion.hlsl"
    VERBATIM
  )
  add_library(asura_temporal_d3d12 OBJECT
    temporal_d3d12.cpp temporal_camera.cpp "${depth_motion_bytecode}"
  )
  target_include_directories(asura_temporal_d3d12 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_include_directories(asura_temporal_d3d12 SYSTEM PRIVATE
    "${fidelityfx_root}/api/include"
    "${fidelityfx_root}/upscalers/include"
    "${streamline_include}"
  )
  target_compile_definitions(asura_temporal_d3d12 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(ASURA_TEMPORAL_PROBE)
    add_executable(temporal_probe upscale_probe.cpp)
    target_compile_definitions(temporal_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(temporal_probe PRIVATE asura_temporal_d3d12 d3d12)
  endif()
  if(ASURA_LIVE_DLSS_QA)
    include(live_dlss_hooks.cmake)
    set(object_pose_bytecode "${CMAKE_CURRENT_BINARY_DIR}/object_pose_bytecode.h")
    add_custom_command(OUTPUT "${object_pose_bytecode}"
      COMMAND "${ASURA_FXC}" /nologo /O3 /T ps_5_1 /E PSMain
        /Fh "${object_pose_bytecode}" /Vn asura_object_pose
        "${CMAKE_CURRENT_SOURCE_DIR}/object_pose.hlsl"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/object_pose.hlsl"
      VERBATIM)
    set(hud_target_bytecode "${CMAKE_CURRENT_BINARY_DIR}/prepare_hud_target_bytecode.h")
    add_custom_command(OUTPUT "${hud_target_bytecode}"
      COMMAND "${ASURA_FXC}" /nologo /O3 /T cs_5_1 /E CSMain
        /Fh "${hud_target_bytecode}" /Vn asura_prepare_hud_target
        "${CMAKE_CURRENT_SOURCE_DIR}/prepare_hud_target.hlsl"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prepare_hud_target.hlsl"
      VERBATIM)
    set(transition_bytecode "${CMAKE_CURRENT_BINARY_DIR}/copy_transition_bytecode.h")
    add_custom_command(OUTPUT "${transition_bytecode}"
      COMMAND "${ASURA_FXC}" /nologo /O3 /T cs_5_1 /E CopyTransition
        /Fh "${transition_bytecode}" /Vn asura_copy_transition
        "${CMAKE_CURRENT_SOURCE_DIR}/prepare_hud_target.hlsl"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prepare_hud_target.hlsl"
      VERBATIM)
    set(reactive_bytecode "${CMAKE_CURRENT_BINARY_DIR}/fsr3_reactive_bytecode.h")
    add_custom_command(OUTPUT "${reactive_bytecode}"
      COMMAND "${ASURA_FXC}" /nologo /O3 /T cs_5_1 /E CSMain
        /Fh "${reactive_bytecode}" /Vn asura_fsr3_reactive
        "${CMAKE_CURRENT_SOURCE_DIR}/fsr3_reactive.hlsl"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fsr3_reactive.hlsl"
      VERBATIM)
    target_sources(asura_d3d12_trace PRIVATE live_dlss_qa.cpp "${object_pose_bytecode}"
                   "${hud_target_bytecode}" "${transition_bytecode}" "${reactive_bytecode}")
    target_include_directories(asura_d3d12_trace PRIVATE
      "${sdk_source}/thirdparty/dxbc" "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(asura_d3d12_trace PRIVATE asura_temporal_d3d12)
  endif()
endif()
