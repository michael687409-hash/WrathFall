function(replace_hook variable anchor hook)
  string(FIND "${${variable}}" "${anchor}" offset)
  if(offset EQUAL -1)
    message(FATAL_ERROR "The pinned Vulkan hook was not found: ${anchor}")
  endif()
  string(REPLACE "${anchor}" "${hook}" code "${${variable}}")
  set(${variable} "${code}" PARENT_SCOPE)
endfunction()

foreach(name command_processor render_target_cache)
  set(source "${xenos}/vulkan/${name}.cpp")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
  file(READ "${source}" ${name})
endforeach()

foreach(name vulkan_instance vulkan_device)
  set(source "${xenos}/host/vulkan/${name}.cpp")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
  file(READ "${source}" ${name})
endforeach()

set(anchor [=[  // Truncating to this size discards layer provided extensions when layers are absent.]=])
set(hook [=[
  wrathfall::vulkan_live::PrepareInstance(vulkan_instance->api_version_,
                                         supported_implementation_extensions, enabled_extensions);
  // Truncating to this size discards layer provided extensions when layers are absent.]=])
replace_hook(vulkan_instance "${anchor}" "${hook}")
set(anchor [=[VulkanInstance::~VulkanInstance() {
  if (instance_) {]=])
set(hook [=[VulkanInstance::~VulkanInstance() {
  wrathfall::vulkan_live::ReleaseInstance();
  if (instance_) {]=])
replace_hook(vulkan_instance "${anchor}" "${hook}")

set(anchor [=[  std::vector<const char*> enabled_extensions;
  {]=])
set(hook [=[  std::vector<const char*> enabled_extensions;
  std::vector<VkExtensionProperties> supported_extensions;
  {]=])
replace_hook(vulkan_device "${anchor}" "${hook}")
set(anchor [=[      std::vector<VkExtensionProperties> supported_extensions(supported_extension_count);]=])
set(hook [=[      supported_extensions.resize(supported_extension_count);]=])
replace_hook(vulkan_device "${anchor}" "${hook}")
set(anchor [=[  const VkResult device_create_result =
      ifn.vkCreateDevice(physical_device, &device_create_info, nullptr, &device->device_);]=])
set(hook [=[
  if (wrathfall::vulkan_live::TemporalRequested()) {
    enabled_features.shaderStorageImageExtendedFormats = supported_features.shaderStorageImageExtendedFormats;
  }
  const auto native_features12 = features_1_2.enabled;
  const auto native_features13 = features_1_3.enabled;
  const auto native_extension_count = device_create_info.enabledExtensionCount;
  bool dlss_device = with_gpu_emulation &&
      wrathfall::vulkan_live::PrepareDevice(physical_device, properties, supported_extensions,
          features_1_2.supported, features_1_3.supported, features_1_2.enabled, features_1_3.enabled,
          enabled_extensions);
  if (dlss_device) {
    device_create_info.enabledExtensionCount = static_cast<u32>(enabled_extensions.size());
    device_create_info.ppEnabledExtensionNames = enabled_extensions.data();
  }
  VkResult device_create_result =
      ifn.vkCreateDevice(physical_device, &device_create_info, nullptr, &device->device_);
  if (device_create_result != VK_SUCCESS && dlss_device) {
    REXUI_WARN("Vulkan device creation with DLSS failed ({}). Retrying native Vulkan.",
               static_cast<int>(device_create_result));
    features_1_2.enabled = native_features12;
    features_1_3.enabled = native_features13;
    device_create_info.enabledExtensionCount = native_extension_count;
    dlss_device = false;
    device_create_result =
        ifn.vkCreateDevice(physical_device, &device_create_info, nullptr, &device->device_);
  }]=])
replace_hook(vulkan_device "${anchor}" "${hook}")
set(anchor [=[  return device;
}

VulkanDevice::~VulkanDevice() {
  if (device_) {]=])
set(hook [=[  if (dlss_device) {
    wrathfall::vulkan_live::BindDevice(vulkan_instance->GetInstance(), physical_device,
                                      device->device_, device->GetQueueFamilyGraphicsCompute());
  }
  return device;
}

VulkanDevice::~VulkanDevice() {
  if (device_) {
    wrathfall::vulkan_live::ReleaseDevice(device_);]=])
replace_hook(vulkan_device "${anchor}" "${hook}")

foreach(name vulkan_instance vulkan_device)
  set(first_include "#include \"gpu/xenos/host/vulkan/${name}.h\"")
  replace_hook(${name} "${first_include}" "${first_include}\n#include \"live_scene.h\"")
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp")
  file(CONFIGURE OUTPUT "${generated}" CONTENT "${${name}}" @ONLY)
  list(REMOVE_ITEM vulkan_sources "${xenos}/host/vulkan/${name}.cpp")
  list(APPEND vulkan_sources "${generated}")
endforeach()

set(anchor [=[  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();]=])
set(hook [=[
  wrathfall::vulkan_live::Device(vulkan_device->GetPhysicalDevice(), device,
      vulkan_device->GetVulkanInstance()->GetFunctions().vkGetDeviceProcAddr,
      vulkan_device->GetQueueFamilyGraphicsCompute());
  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();]=])
replace_hook(command_processor "${anchor}" "${hook}")

set(anchor [=[void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();]=])
set(hook [=[void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  wrathfall::vulkan_live::Shutdown(*this);]=])
replace_hook(command_processor "${anchor}" "${hook}")

set(anchor [=[  // The frame ends even without presenting, otherwise per frame resources leak.
  EndSubmission(true);]=])
set(hook [=[  // The frame ends even without presenting, otherwise per frame resources leak.
  EndSubmission(true);
  if (wrathfall::vulkan_live::CaptureReady() && AwaitAllQueueOperationsCompletion()) {
    wrathfall::vulkan_live::ReadInterlockInputs(*this);
  }]=])
replace_hook(command_processor "${anchor}" "${hook}")

set(anchor [=[  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);]=])
set(hook [=[
  if (wrathfall::vulkan_live::Enabled() && draw_resolution_scale_x == 1 &&
      draw_resolution_scale_y == 1 && pixel_shader && normalized_color_mask) {
    std::array<float, 16> camera{};
    const auto address = std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X);
    std::memcpy(camera.data(), &regs[address], sizeof(camera));
    const auto color =
        regs.Get<xenos::reg::RB_COLOR_INFO>(xenos::reg::RB_COLOR_INFO::kRTRegisterIndices[0]);
    const auto surface = regs.Get<xenos::reg::RB_SURFACE_INFO>();
    if (wrathfall::vulkan_live::Draw(frame_current_, vertex_shader->GetUcodeDataHash(),
        pixel_shader->GetUcodeDataHash(), viewport_info.xy_extent[0], viewport_info.xy_extent[1],
        color.color_base, regs.Get<xenos::reg::RB_DEPTH_INFO>().depth_base,
        host_render_targets_used, normalized_depth_control.z_enable,
        normalized_depth_control.stencil_enable, camera, viewport_info.z_min, viewport_info.z_max,
        static_cast<u32>(color.color_format), surface.surface_pitch,
        1u << static_cast<unsigned>(surface.msaa_samples))) {
      wrathfall::vulkan_live::CopyInterlockInput(*this, frame_current_,
                                                render_target_cache_->GetEDRAMBuffer(), false);
    }
    if (!host_render_targets_used && normalized_depth_control.z_enable && color.color_base != 720 &&
        regs.Get<xenos::reg::RB_DEPTH_INFO>().depth_base == 0 &&
        viewport_info.xy_extent[0] == 1280 && viewport_info.xy_extent[1] == 720) {
      const auto jitter = wrathfall::vulkan_live::Jitter(frame_current_);
      viewport_info.ndc_offset[0] += jitter[0] * (2.0f / 1280.0f);
      viewport_info.ndc_offset[1] += jitter[1] * (2.0f / 720.0f);
    }
  }

  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);]=])
replace_hook(command_processor "${anchor}" "${hook}")

set(anchor [=[  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,]=])
set(hook [=[
  const bool temporal_frontbuffer = frontbuffer_format == xenos::TextureFormat::k_8_8_8_8;
  if (wrathfall::vulkan_live::CanPresentTemporal(frame_current_,
        temporal_frontbuffer ? guest_output_width : 0, guest_output_height)) {
    const bool swap_red_blue = swap_source_needs_rb_swap &&
        !GetVulkanDevice()->GetProperties().imageViewFormatSwizzle;
    if (presenter->RefreshGuestOutput(2560, 1440, display_width, display_height,
          [&](gpu::Presenter::GuestOutputRefreshContext& context) -> bool {
            if (!BeginSubmission(true)) return false;
            auto& output = static_cast<gpu::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
            const auto* lut = GetGammaRamp256EntryTable();
            std::array<uint32_t, 256> gamma{};
            for (size_t i = 0; i < gamma.size(); ++i) {
              gamma[i] = uint32_t(lut[i].color_10_red) |
                  (uint32_t(lut[i].color_10_green) << 10) | (uint32_t(lut[i].color_10_blue) << 20);
            }
            if (!wrathfall::vulkan_live::ScheduleTemporal(frame_current_, output.GetImage(),
                    output.GetImageView(), output.HasImageBeenWritten(), swap_texture_view,
                    frontbuffer_width_scaled, frontbuffer_height_scaled, swap_red_blue, gamma)) return false;
            context.SetIs8bpc(false);
            return EndSubmission(true);
          })) return;
  }

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,]=])
replace_hook(command_processor "${anchor}" "${hook}")

set(anchor [=[    deferred_command_buffer_.Execute(command_buffer.buffer);]=])
set(hook [=[    deferred_command_buffer_.Execute(command_buffer.buffer);
    wrathfall::vulkan_live::RecordTemporal(command_buffer.buffer);]=])
replace_hook(command_processor "${anchor}" "${hook}")

set(anchor [=[  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&]=])
set(hook [=[
  wrathfall::vulkan_live::Present(frame_current_, frontbuffer_ptr,
                                   guest_output_width, guest_output_height);
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&]=])
replace_hook(command_processor "${anchor}" "${hook}")

set(anchor [=[  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {]=])
set(hook [=[
  if (wrathfall::vulkan_live::Enabled() && !draw_resolution_scaled &&
      resolve_info.IsCopyingDepth() && resolve_info.depth_original_base == 0 &&
      resolve_info.copy_dest_base == 0x14978000 &&
      resolve_info.coordinate_info.width_div_8 == 160 && resolve_info.height_div_8 == 90) {
    const auto* target = GetPath() == Path::kHostRenderTargets
        ? static_cast<VulkanRenderTarget*>(GetLastUpdateAccumulatedRenderTargets()[0]) : nullptr;
    const bool image = target && target->GetKey().base_tiles == 0 &&
                       target->GetViewDepthColor() != VK_NULL_HANDLE;
    const auto& layout = resolve_info.depth_edram_info;
    if (wrathfall::vulkan_live::DepthResolve(command_processor_.GetCurrentFrame(),
        GetPath() == Path::kPixelShaderInterlock, image,
        1u << static_cast<unsigned>(layout.msaa_samples), layout.format, layout.pitch_tiles,
        layout.base_tiles, resolve_info.copy_dest_extent_length)) {
      wrathfall::vulkan_live::CopyInterlockInput(command_processor_, command_processor_.GetCurrentFrame(),
                                                edram_buffer_, true);
    }
  }
  if (wrathfall::vulkan_live::TemporalRequested() && !draw_resolution_scaled &&
      GetPath() == Path::kPixelShaderInterlock && !resolve_info.IsCopyingDepth() &&
      resolve_info.copy_dest_base == 0x1f5f8000 && resolve_info.color_original_base == 720 &&
      resolve_info.coordinate_info.width_div_8 == 160 && resolve_info.height_div_8 == 90 &&
      resolve_info.copy_dest_extent_length &&
      wrathfall::vulkan_live::NeedsColorBeforeResolve(command_processor_.GetCurrentFrame())) {
    wrathfall::vulkan_live::CopyInterlockInput(command_processor_, command_processor_.GetCurrentFrame(),
                                              edram_buffer_, false);
  }

  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {]=])
replace_hook(render_target_cache "${anchor}" "${hook}")

foreach(name command_processor render_target_cache)
  string(REPLACE "#include \"../shaders/" "#include \"shaders/" ${name} "${${name}}")
  set(first_include "#include \"gpu/xenos/vulkan/${name}.h\"")
  replace_hook(${name} "${first_include}" "${first_include}\n#include \"live_scene.h\"")
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp")
  file(CONFIGURE OUTPUT "${generated}" CONTENT "${${name}}" @ONLY)
  list(REMOVE_ITEM vulkan_sources "${xenos}/vulkan/${name}.cpp")
  list(APPEND vulkan_sources "${generated}")
endforeach()
