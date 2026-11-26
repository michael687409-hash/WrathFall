# Keep the pinned SDK sources intact. Hooks are compiled into the isolated QA DLL.
function(asura_replace_once text anchor replacement)
  string(FIND "${${text}}" "${anchor}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Could not find the pinned D3D12 hook in ${text}: ${anchor}")
  endif()
  string(REPLACE "${anchor}" "${replacement}" patched "${${text}}")
  set(${text} "${patched}" PARENT_SCOPE)
endfunction()

function(asura_read_sdk path out)
  set(absolute "${xenos_source}/${path}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${absolute}")
  file(READ "${absolute}" input)
  set(${out} "${input}" PARENT_SCOPE)
endfunction()

asura_read_sdk("d3d12/render_target_cache.h" rt_header)
set(anchor [=[  bool IsGammaRenderTargetAsUnorm16() const]=])
set(hook [=[  bool AsuraCopySceneTarget(bool depth);
  bool AsuraNeutralizeBlurTarget();

  bool IsGammaRenderTargetAsUnorm16() const]=])
asura_replace_once(rt_header "${anchor}" "${hook}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/qa/gpu/xenos/d3d12/render_target_cache.h"
     CONTENT "${rt_header}" @ONLY)

set(anchor [=[bool D3D12RenderTargetCache::Resolve(IPhysicalMemory& memory]=])
set(hook [=[
bool D3D12RenderTargetCache::AsuraCopySceneTarget(bool depth) {
  using namespace asura::temporal::live;
  if (!Active() || GetPath() != Path::kHostRenderTargets ||
      GetDrawResolutionScaleX() != 1 || GetDrawResolutionScaleY() != 1) return false;
  const size_t slot = depth ? 0 : 1;
  auto* target = static_cast<D3D12RenderTarget*>(GetLastUpdateAccumulatedRenderTargets()[slot]);
  if (!target || target->GetKey().base_tiles != (depth ? 0u : 720u)) return false;
  auto* source = target->GetResource();
  auto* device = command_processor_.GetD3D12Provider().GetDevice();
  const auto frame = command_processor_.GetCurrentFrame();
  auto* dest = depth ? CaptureDepth(device, frame, source) : CaptureColor(device, frame, source);
  if (!dest) return false;
  constexpr auto copy = D3D12_RESOURCE_STATE_COPY_SOURCE;
  const auto prior = target->SetResourceState(copy);
  command_processor_.PushTransitionBarrier(source, prior, copy);
  command_processor_.SubmitBarriers();
  auto& commands = command_processor_.GetDeferredCommandList();
  if (depth) {
    commands.D3DCopyResource(dest, source);
  } else {
    const D3D12_BOX rectangle{0, 0, 0, 1280, 720, 1};
    const D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    const D3D12_TEXTURE_COPY_LOCATION to{dest, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    commands.D3DCopyTextureRegion(&to, 0, 0, 0, &from, &rectangle);
  }
  command_processor_.PushTransitionBarrier(source, copy, prior);
  target->SetResourceState(prior);
  return true;
}

bool D3D12RenderTargetCache::AsuraNeutralizeBlurTarget() {
  if (GetPath() != Path::kHostRenderTargets || IsDrawResolutionScaled()) return false;
  auto* target = static_cast<D3D12RenderTarget*>(GetLastUpdateAccumulatedRenderTargets()[1]);
  if (!target || target->GetKey().base_tiles != 1624 ||
      target->GetKey().GetColorFormat() != xenos::ColorRenderTargetFormat::k_8_8_8_8) return false;
  const auto desc = target->GetResource()->GetDesc();
  if (desc.SampleDesc.Count != 1 || desc.Width < 640 || desc.Height < 360) return false;

  // Object-pose replay has already recorded a separate target. Clear just
  // the live rectangle of the title's blur resolve to its neutral XY value.
  constexpr auto render_target = D3D12_RESOURCE_STATE_RENDER_TARGET;
  command_processor_.PushTransitionBarrier(target->GetResource(),
      target->SetResourceState(render_target), render_target);
  command_processor_.SubmitBarriers();
  constexpr float neutral[]{0.5f, 0.5f, 0.0f, 0.0f};
  constexpr D3D12_RECT region{0, 0, 640, 360};
  command_processor_.GetDeferredCommandList().D3DClearRenderTargetView(
      target->GetDescriptorDraw().GetHandle(), neutral, 1, &region);
  return true;
}

bool D3D12RenderTargetCache::Resolve(IPhysicalMemory& memory]=])
asura_replace_once(resolve_text "${anchor}" "${hook}")
set(anchor [=[  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {]=])
set(hook [=[  if (resolve_info.IsCopyingDepth() && resolve_info.depth_original_base == 0 &&
      resolve_info.copy_dest_base == 0x14978000 &&
      resolve_info.coordinate_info.width_div_8 == 160 &&
      resolve_info.height_div_8 == 90) {
    AsuraCopySceneTarget(true);
  }
  asura::temporal::live::TraceResolve(command_processor_.GetCurrentFrame(),
      resolve_info.IsCopyingDepth(), resolve_info.IsCopyingDepth()
          ? resolve_info.depth_original_base : resolve_info.color_original_base,
      resolve_info.copy_dest_base, resolve_info.coordinate_info.width_div_8 * 8,
      resolve_info.height_div_8 * 8);
  if (asura::temporal::live::SuppressMotionBlur() && !resolve_info.IsCopyingDepth() &&
      resolve_info.color_original_base == 1624 && resolve_info.copy_dest_base == 0x13980000 &&
      resolve_info.coordinate_info.width_div_8 == 80 && resolve_info.height_div_8 == 45 &&
      AsuraNeutralizeBlurTarget()) {
    asura::temporal::live::TraceBlurSuppressed(command_processor_.GetCurrentFrame());
  }
  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {]=])
asura_replace_once(resolve_text "${anchor}" "${hook}")
set(anchor [=[          written_length_out = resolve_info.copy_dest_extent_length;
          copied = true;]=])
set(hook [=[          written_length_out = resolve_info.copy_dest_extent_length;
          copied = true;
          if (!resolve_info.IsCopyingDepth() && resolve_info.color_original_base == 720 &&
              resolve_info.copy_dest_base == 0x1F5F8000 &&
              resolve_info.coordinate_info.width_div_8 == 160 && resolve_info.height_div_8 == 90) {
            asura::temporal::live::SealHud(command_processor_.GetCurrentFrame());
          }]=])
asura_replace_once(resolve_text "${anchor}" "${hook}")
string(PREPEND resolve_text "#include \"live_dlss_qa.h\"\n")
file(CONFIGURE OUTPUT "${traced_resolve}" CONTENT "${resolve_text}" @ONLY)

asura_read_sdk("d3d12/command_processor.cpp" processor)
set(anchor [=[  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control);]=])
set(hook [=[
  namespace live = asura::temporal::live;
  bool asura_pre_hud = false;
  bool asura_pose_pass = false;
  if (live::Active() && host_render_targets_used && draw_resolution_scale_x == 1 &&
      draw_resolution_scale_y == 1 && viewport_info.xy_offset[0] == 0 &&
      viewport_info.xy_offset[1] == 0) {
    const auto size_x = viewport_info.xy_extent[0];
    const auto size_y = viewport_info.xy_extent[1];
    const auto color_base =
        regs.Get<xenos::reg::RB_COLOR_INFO>(xenos::reg::RB_COLOR_INFO::kRTRegisterIndices[0])
            .color_base;
    const auto guest_vs = vertex_shader->GetUcodeDataHash();
    const auto read_vertex_view = [&regs]() {
      asura::temporal::Matrix view;
      const auto address = std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X);
      std::memcpy(view.data(), &regs[address], sizeof(view));
      return view;
    };
    if (size_x == 640 && size_y == 360 && pixel_shader_translation && color_base == 1624) {
      const auto& vs = vertex_shader_translation->GetTranslatedBinary();
      const auto& ps = pixel_shader_translation->GetTranslatedBinary();
      const auto guest_ps = pixel_shader->GetUcodeDataHash();
      const bool full = live::IsMotionProgram(guest_vs, guest_ps);
      const bool compact = live::IsCompactMotionProgram(guest_vs, guest_ps);
      asura_pose_pass = full || compact;
      live::ObserveCandidate(frame_current_, false, asura_pose_pass, guest_vs, guest_ps, vs, ps);
      if (asura_pose_pass) {
        std::optional<asura::temporal::Matrix> previous;
        std::optional<asura::temporal::ClipXY> previous_xy;
        std::optional<asura::temporal::ViewOrigin> origin;
        const auto& vertex_map = vertex_shader->GetConstantRegisterMap();
        if (vertex_map.float_count == 256) {
          std::array<float, 4> row;
          const auto address = std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X) + 5 * 4;
          std::memcpy(row.data(), &regs[address], sizeof(row));
          if (row[3] == 1.0f && std::all_of(row.begin(), row.end(), [](float value) {
                return std::isfinite(value) && std::abs(value) < 10000000.0f;
              })) origin = asura::temporal::ViewOrigin{row[0], row[1], row[2]};
        }
        // The full program reads packed pixel rows 0-3. The compact program uses
        // rows 3-6 and only consumes their X/Y components.
        const size_t first = full ? 0 : 3;
        size_t row = 0;
        asura::temporal::Matrix packed{};
        const auto& pixel_map = pixel_shader->GetConstantRegisterMap();
        const auto pixel_base = std::to_underlying(xenos::Register::SHADER_CONSTANT_256_X);
        for (u32 word_index = 0; word_index < 4 && row < first + 4; ++word_index) {
          u64 bits = pixel_map.float_bitmap[word_index];
          while (bits && row < first + 4) {
            const auto bit = std::countr_zero(bits);
            bits &= bits - 1;
            if (row >= first) {
              const auto address = pixel_base + word_index * 256 + bit * 4;
              std::memcpy(packed.data() + (row - first) * 4, &regs[address], 4 * sizeof(float));
            }
            ++row;
          }
        }
        if (row == first + 4) {
          if (full) {
            previous = packed;
          } else {
            previous_xy = asura::temporal::ClipXY{packed[0], packed[1], packed[4], packed[5],
                                                 packed[8], packed[9], packed[12], packed[13]};
          }
          // Use the corresponding 720p host half-pixel correction.
          const float offset[3]{viewport_info.ndc_offset[0] * 0.5f,
                                viewport_info.ndc_offset[1] * 0.5f, viewport_info.ndc_offset[2]};
          live::ObserveCamera(frame_current_, read_vertex_view(), previous, origin, previous_xy,
                              viewport_info.ndc_scale, offset);
        }
      }
    } else if (size_x == 1280 && size_y == 720) {
      if (color_base == 720 && viewport_info.z_max == 1.0f && pixel_shader_translation) {
        const auto& vs = vertex_shader_translation->GetTranslatedBinary();
        const auto& ps = pixel_shader_translation->GetTranslatedBinary();
        const auto guest_ps = pixel_shader->GetUcodeDataHash();
        asura_pre_hud = live::IsSceneCompositor(guest_vs, guest_ps);
        live::ObserveCandidate(frame_current_, true, asura_pre_hud, guest_vs, guest_ps, vs, ps);
      } else if (viewport_info.z_max == 0.5f && normalized_depth_control.z_enable &&
                 regs.Get<xenos::reg::RB_DEPTH_INFO>().depth_base == 0) {
        if (live::IsOpaqueViewProgram(guest_vs)) {
          live::ObserveCamera(frame_current_, read_vertex_view(), std::nullopt, std::nullopt,
                              std::nullopt, viewport_info.ndc_scale, viewport_info.ndc_offset);
        }
        if (normalized_depth_control.z_write_enable || normalized_color_mask) {
          const auto base = std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X);
          live::ObserveOpaqueCamera(frame_current_, guest_vs,
              pixel_shader ? pixel_shader->GetUcodeDataHash() : 0,
              vertex_shader->GetConstantRegisterMap().float_bitmap[0], {&regs[base], 32 * 4},
              viewport_info.ndc_scale, viewport_info.ndc_offset);
        }
        const auto jitter = live::Jitter(frame_current_);
        viewport_info.ndc_offset[0] += jitter.x * (2.0f / 1280.0f);
        viewport_info.ndc_offset[1] -= jitter.y * (2.0f / 720.0f);
      }
    }
  }
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control);]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[  // Must not call anything that may change the primitive topology from now on.]=])
set(hook [=[  // Must not call anything that may change the primitive topology from now on.
  const auto asura_record_hud = [&](const auto& draw) {
    if (!normalized_color_mask || !live::HudActive(frame_current_)) return;
    const auto& color = regs.Get<xenos::reg::RB_COLOR_INFO>(
        xenos::reg::RB_COLOR_INFO::kRTRegisterIndices[0]);
    const u64 vs = vertex_shader->GetUcodeDataHash();
    const u64 ps = pixel_shader ? pixel_shader->GetUcodeDataHash() : 0;
    const bool eligible = !memexport_used && !active_occlusion_query_.valid &&
                     !normalized_depth_control.z_enable && !normalized_depth_control.stencil_enable &&
                     host_render_targets_used && (bound_depth_and_color_render_target_bits & ~1u) == 2 &&
                     (normalized_color_mask & ~15u) == 0 &&
                     color.color_base == 720 &&
                     bound_depth_and_color_render_target_formats[1] ==
                         static_cast<u32>(xenos::ColorRenderTargetFormat::k_8_8_8_8) &&
                     ff_viewport_.Width == 1280.0f && ff_viewport_.Height == 720.0f;
    const bool texture_blend = live::IsHudTextureBlend(vs, ps);
    bool supported = eligible && live::IsHudProgram(vs, ps);
    if (texture_blend) {
      std::array<uint32_t, 8> sources{};
      supported = eligible && used_texture_mask == 3;
      for (u32 index = 0; index != 2; ++index) {
        const auto fetch = regs.GetTextureFetch(index);
        u32 width = 0, height = 0, page = 0;
        xenos::GetSubresourcesFromFetchConstant(
            fetch, &width, &height, nullptr, &page, nullptr, nullptr, nullptr);
        sources[index * 4] = page;
        sources[index * 4 + 1] = width + 1;
        sources[index * 4 + 2] = height + 1;
        sources[index * 4 + 3] = static_cast<u32>(fetch.format);
        supported &= page && width < 512 && height < 512;
      }
      live::TraceHudTextureBlend(frame_current_, sources, eligible, supported);
    }
    // Reject overlays that read back the guest frontbuffer.
    for (u32 textures = supported ? used_texture_mask : 0; textures; textures &= textures - 1) {
      const u32 index = std::countr_zero(textures);
      const auto fetch = regs.GetTextureFetch(index);
      u32 page = 0;
      xenos::GetSubresourcesFromFetchConstant(
          fetch, nullptr, nullptr, nullptr, &page, nullptr, nullptr, nullptr);
      if (page == (0x1F5F8000u >> 12)) {
        supported = false;
        break;
      }
    }
    const auto target = live::HudTarget(frame_current_, supported, vs, ps);
    if (!target) return;
    auto viewport = ff_viewport_;
    viewport.TopLeftX *= 2;
    viewport.TopLeftY *= 2;
    viewport.Width *= 2;
    viewport.Height *= 2;
    const D3D12_RECT rect{ff_scissor_.left * 2, ff_scissor_.top * 2,
                          ff_scissor_.right * 2, ff_scissor_.bottom * 2};
    deferred_command_list_.D3DOMSetRenderTargets(1, &*target, FALSE, nullptr);
    deferred_command_list_.D3DRSSetViewport(viewport);
    deferred_command_list_.D3DRSSetScissorRect(rect);
    draw();
    deferred_command_list_.D3DOMSetRenderTargets(0, nullptr, FALSE, nullptr);
    deferred_command_list_.D3DRSSetViewport(ff_viewport_);
    deferred_command_list_.D3DRSSetScissorRect(ff_scissor_);
    render_target_cache_->InvalidateCommandListRenderTargets();
  };
  const auto asura_record_pose = [&](const auto& draw) {
    if (!asura_pose_pass || memexport_used || active_occlusion_query_.valid ||
        primitive_topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) return;
    const auto pose = live::BeginObjectPose(GetD3D12Provider().GetDevice(), frame_current_,
                                          root_signature, vertex_shader->GetUcodeDataHash(),
                                          vertex_shader_translation->GetTranslatedBinary());
    if (!pose) return;
    constexpr auto depth_read = D3D12_RESOURCE_STATE_DEPTH_READ;
    constexpr auto copy_dest = D3D12_RESOURCE_STATE_COPY_DEST;
    PushTransitionBarrier(pose->depth, copy_dest, depth_read);
    SubmitBarriers();
    if (pose->clear) {
      constexpr float empty[4] = {};
      deferred_command_list_.D3DClearRenderTargetView(pose->rtv, empty, 0, nullptr);
    }
    const D3D12_RECT rect{ff_scissor_.left * 2, ff_scissor_.top * 2,
                          ff_scissor_.right * 2, ff_scissor_.bottom * 2};
    deferred_command_list_.D3DOMSetRenderTargets(1, &pose->rtv, FALSE, &pose->dsv);
    deferred_command_list_.D3DRSSetViewport(pose->viewport);
    deferred_command_list_.D3DRSSetScissorRect(rect);
    deferred_command_list_.D3DSetPipelineState(pose->pipeline);
    draw();
    deferred_command_list_.D3DOMSetRenderTargets(0, nullptr, FALSE, nullptr);
    PushTransitionBarrier(pose->depth, depth_read, copy_dest);
    deferred_command_list_.SetPipelineStateHandle(pipeline_handle);
    deferred_command_list_.D3DRSSetViewport(ff_viewport_);
    deferred_command_list_.D3DRSSetScissorRect(ff_scissor_);
    render_target_cache_->InvalidateCommandListRenderTargets();
  };]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[    deferred_command_list_.D3DDrawInstanced(primitive_processing_result.host_draw_vertex_count, 1,
                                            0, 0);]=])
set(hook [=[    const auto draw = [&] {
      deferred_command_list_.D3DDrawInstanced(primitive_processing_result.host_draw_vertex_count,
                                              1, 0, 0);
    };
    draw();
    asura_record_hud(draw);
    asura_record_pose(draw);]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[    deferred_command_list_.D3DDrawIndexedInstanced(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);]=])
set(hook [=[    const auto draw = [&] {
      deferred_command_list_.D3DDrawIndexedInstanced(
          primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
    };
    draw();
    asura_record_hud(draw);
    asura_record_pose(draw);]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[  if (memexport_used) {
    shared_memory_->MarkUAVWritesCommitNeeded();]=])
set(hook [=[  if (asura_pre_hud && !memexport_used && !active_occlusion_query_.valid &&
      render_target_cache_->AsuraCopySceneTarget(false)) {
    // End the scene submission here. The next guest draw records HUD work.
    EndSubmission(false);
  }

  if (memexport_used) {
    shared_memory_->MarkUAVWritesCommitNeeded();]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[  const VideoMode video_mode = title_info_.video_mode;]=])
set(hook [=[  ID3D12Resource* asura_presentation = nullptr;
  u32 asura_page = 0;
  bool asura_frontbuffer_supported = false;
  if (asura::temporal::live::Active()) {
    const auto fetch = register_file_->GetTextureFetch(0);
    xenos::GetSubresourcesFromFetchConstant(
        fetch, nullptr, nullptr, nullptr, &asura_page, nullptr, nullptr, nullptr);
    asura_frontbuffer_supported = asura_page == (0x1F5F8000u >> 12) &&
        guest_output_width == 1280 && guest_output_height == 720 &&
        frontbuffer_format == xenos::TextureFormat::k_8_8_8_8;
    if (asura_frontbuffer_supported) {
      asura_presentation = asura::temporal::live::PresentHud(frame_current_);
      if (!asura_presentation) {
        auto* transition = asura::temporal::live::CaptureComposedTransition(
            frame_current_, swap_texture_resource, swap_texture_srv_desc);
        if (transition) {
          constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          constexpr auto copy = D3D12_RESOURCE_STATE_COPY_SOURCE;
          PushTransitionBarrier(swap_texture_resource, read, copy);
          SubmitBarriers();
          const D3D12_TEXTURE_COPY_LOCATION from{
              swap_texture_resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
          const D3D12_TEXTURE_COPY_LOCATION to{
              transition, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
          const D3D12_BOX crop{0, 0, 0, 1280, 720, 1};
          deferred_command_list_.D3DCopyTextureRegion(&to, 0, 0, 0, &from, &crop);
          PushTransitionBarrier(swap_texture_resource, copy, read);
          if (!EndSubmission(false) || !BeginSubmission(true)) return;
          asura_presentation = asura::temporal::live::PresentHud(frame_current_);
        }
      }
    }
  }
  asura::temporal::live::TraceSwap(frame_current_, asura_page, uint32_t(frontbuffer_format),
      guest_output_width, guest_output_height, asura_frontbuffer_supported,
      asura_presentation != nullptr);
  if (asura_presentation) {
    swap_texture_resource = asura_presentation;
    swap_texture_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_texture_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const auto desc = asura_presentation->GetDesc();
    guest_output_width = static_cast<u32>(desc.Width);
    guest_output_height = desc.Height;
  }

  const VideoMode video_mode = title_info_.video_mode;]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[       guest_output_height](gpu::Presenter::GuestOutputRefreshContext& context) -> bool {]=])
set(hook [=[       guest_output_height, asura_presentation](gpu::Presenter::GuestOutputRefreshContext& context) -> bool {]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[        bool use_fxaa = swap_post_effect == SwapPostEffect::kFXAA ||
                        swap_post_effect == SwapPostEffect::kFXAAExtreme;]=])
set(hook [=[        bool use_fxaa = !asura_presentation &&
                        (swap_post_effect == SwapPostEffect::kFXAA ||
                         swap_post_effect == SwapPostEffect::kFXAAExtreme);]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[        device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,]=])
set(hook [=[        if (asura_presentation) {
          PushTransitionBarrier(asura_presentation, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[        deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);

        if (use_fxaa) {]=])
set(hook [=[        deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
        if (asura_presentation) {
          PushTransitionBarrier(asura_presentation, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
          asura::temporal::live::ObserveHudPresent(frame_current_);
        }

        if (use_fxaa) {]=])
asura_replace_once(processor "${anchor}" "${hook}")
set(anchor [=[    deferred_command_list_.Execute(command_list_, command_list_1_);]=])
set(hook [=[    deferred_command_list_.Execute(command_list_, command_list_1_);
    asura::temporal::live::Record(command_list_, submission_current_, submission_completed_);]=])
asura_replace_once(processor "${anchor}" "${hook}")
# The provider lives longer than the command processor's drained GPU queue.
string(REPLACE "#include \"../shaders/" "#include \"shaders/" processor "${processor}")
string(PREPEND processor "#include \"live_dlss_qa.h\"\n#include <bit>\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/qa_command_processor.cpp" CONTENT "${processor}" @ONLY)

asura_read_sdk("host/d3d12/d3d12_provider.cpp" provider)
set(anchor [=[bool D3D12Provider::Initialize(i32 adapter_index_wanted) {]=])
set(hook [=[bool D3D12Provider::Initialize(i32 adapter_index_wanted) {
  asura::temporal::live::Prepare();]=])
asura_replace_once(provider "${anchor}" "${hook}")
set(anchor [=[D3D12Provider::~D3D12Provider() {]=])
set(hook [=[D3D12Provider::~D3D12Provider() {
  asura::temporal::live::Shutdown();]=])
asura_replace_once(provider "${anchor}" "${hook}")
string(PREPEND provider "#include \"live_dlss_qa.h\"\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/qa_d3d12_provider.cpp" CONTENT "${provider}" @ONLY)

set_source_files_properties(
  "${xenos_source}/d3d12/command_processor.cpp"
  "${xenos_source}/host/d3d12/d3d12_provider.cpp" PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(asura_d3d12_trace PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/qa_command_processor.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/qa_d3d12_provider.cpp")
target_include_directories(asura_d3d12_trace BEFORE PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/qa" "${CMAKE_CURRENT_SOURCE_DIR}")
