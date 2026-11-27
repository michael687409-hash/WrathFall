# Build the pinned ReXGlue UI with this project's prompt implementation.
set(rex_ui_source "${PROJECT_SOURCE_DIR}/.tools/rexglue-sdk-0.11.0/src/ui")
if(NOT EXISTS "${rex_ui_source}/ui_system.cpp")
  message(FATAL_ERROR "The ReXGlue 0.11.0 source archive is required under .tools/rexglue-sdk-0.11.0.")
endif()

set(rex_ui_sources
  imgui_host.cpp
  module_entry.cpp
  overlay/achievement_icon_cache.cpp
  overlay/achievement_toast.cpp
  overlay/achievement_overlay.cpp
  overlay/bind_table.cpp
  overlay/console/model.cpp
  overlay/console/view.cpp
  overlay/console_command_table.cpp
  overlay/console_host.cpp
  overlay/console_overlay.cpp
  overlay/debug_overlay.cpp
  overlay/image_decode.cpp
  overlay/overlay_registry.cpp
  overlay/settings_overlay.cpp
  style.cpp
  ui_event.cpp
  ui_module.cpp
  ui_system.cpp
  video_mode.cpp
  window_settings.cpp
)
list(TRANSFORM rex_ui_sources PREPEND "${rex_ui_source}/")

add_library(asura_console_ui SHARED ${rex_ui_sources} prompt.cpp prompt_controller.cpp)
target_compile_definitions(asura_console_ui PRIVATE REX_BUILDING_UI REXGLUE_BUILD_CONFIG="$<CONFIG>")
target_include_directories(asura_console_ui PRIVATE "${rex_ui_source}/..")
target_include_directories(asura_console_ui SYSTEM PRIVATE "${rex_ui_source}/../../thirdparty/stb")
target_link_libraries(asura_console_ui PRIVATE rex::base SDL3::SDL3)
rexglue_link_host_allocator(asura_console_ui)
set_target_properties(asura_console_ui PROPERTIES
  OUTPUT_NAME rexui DEBUG_POSTFIX d RELWITHDEBINFO_POSTFIX rd
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime/$<CONFIG>"
)

add_custom_target(asuras_runtime ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:asura_console_ui>" "$<TARGET_FILE_DIR:asuras_wrath>/$<TARGET_FILE_NAME:asura_console_ui>"
  DEPENDS asuras_wrath asura_console_ui
  VERBATIM
)

add_executable(prompt_controller_check EXCLUDE_FROM_ALL prompt_controller_check.cpp prompt_controller.cpp)
target_compile_definitions(prompt_controller_check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(prompt_controller_check PRIVATE SDL3::SDL3)
set_target_properties(prompt_controller_check PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)
