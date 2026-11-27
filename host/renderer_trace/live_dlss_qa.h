#pragma once

#include "temporal_camera.h"

#include <span>

namespace asura::temporal::live {

// These hooks are present only in the isolated renderer build.
void Prepare() noexcept;
void Shutdown() noexcept;
bool Active() noexcept;
bool IsMotionProgram(uint64_t vertex, uint64_t pixel);
bool IsOpaqueViewProgram(uint64_t vertex) noexcept;
bool IsCompactMotionProgram(uint64_t vertex, uint64_t pixel) noexcept;
bool IsSceneCompositor(uint64_t vertex, uint64_t pixel);
PixelJitter Jitter(uint64_t frame);
void ObserveCandidate(uint64_t frame, bool compositor, bool match,
                      uint64_t guest_vs, uint64_t guest_ps, std::span<const uint8_t> vertex,
                      std::span<const uint8_t> pixel);
bool SuppressMotionBlur() noexcept;
void TraceBlurSuppressed(uint64_t frame) noexcept;
void TraceResolve(uint64_t frame, bool depth, uint32_t tile, uint32_t address,
                  uint32_t width, uint32_t height) noexcept;
void TraceSwap(uint64_t frame, uint32_t page, uint32_t format,
               uint32_t width, uint32_t height, bool compatible_frontbuffer, bool upscaled) noexcept;

void ObserveCamera(uint64_t frame, const Matrix& current, const std::optional<Matrix>& previous,
                   const std::optional<ViewOrigin>& origin, const std::optional<ClipXY>& previous_xy,
                   const float* scale, const float* offset) noexcept;
void ObserveOpaqueCamera(uint64_t frame, uint64_t shader, uint64_t pixel, uint64_t used_registers,
                         std::span<const uint32_t> registers, const float* scale,
                         const float* offset) noexcept;
ID3D12Resource* CaptureDepth(ID3D12Device* device, uint64_t frame,
                             ID3D12Resource* source) noexcept;
ID3D12Resource* CaptureColor(ID3D12Device* device, uint64_t frame,
                             ID3D12Resource* source) noexcept;
ID3D12Resource* CaptureComposedTransition(uint64_t frame, ID3D12Resource* source,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& view) noexcept;
struct ObjectPoseDraw {
  ID3D12PipelineState* pipeline;
  ID3D12Resource* depth;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;
  D3D12_VIEWPORT viewport;
  bool clear;
};

std::optional<ObjectPoseDraw> BeginObjectPose(ID3D12Device* device, uint64_t frame,
    ID3D12RootSignature* root, uint64_t guest_vs, std::span<const uint8_t> vertex) noexcept;

bool HudActive(uint64_t frame) noexcept;
bool IsHudProgram(uint64_t vertex, uint64_t pixel) noexcept;
bool IsHudTextureBlend(uint64_t vertex, uint64_t pixel) noexcept;
void TraceHudTextureBlend(uint64_t frame, std::span<const uint32_t, 8> sources,
                          bool eligible, bool supported);
std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> HudTarget(uint64_t frame, bool supported,
                                                     uint64_t vertex, uint64_t pixel) noexcept;
void SealHud(uint64_t frame) noexcept;
ID3D12Resource* PresentHud(uint64_t frame) noexcept;
void ObserveHudPresent(uint64_t frame) noexcept;

void Record(ID3D12GraphicsCommandList* list, uint64_t submission,
            uint64_t completed) noexcept;

}  // namespace asura::temporal::live
