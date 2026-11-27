#pragma once

#include "temporal_d3d12.h"

#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>

namespace asura::temporal {

using Matrix = std::array<float, 16>;
using ViewOrigin = std::array<float, 3>;
using ClipXY = std::array<float, 8>;

Matrix RebasePreviousView(Matrix previous, const ViewOrigin& from, const ViewOrigin& to,
                          float origin_sign);
bool MatchesPreviousXY(const Matrix& previous, const ClipXY& xy);

struct PixelJitter {
  float x;
  float y;
};

struct CameraFrame {
  Matrix clip_to_previous_clip;
  Matrix previous_clip_to_clip;
  PixelJitter jitter;
  bool reset;
};

struct DepthRange {
  float min;
  float max;
};

// DepthMotion.hlsl consumes these as root constants. The renderer passes the same
// jitter to the scene projection and the temporal upscaler.
struct DepthMotionConstants {
  Matrix clip_to_previous_clip;
  uint32_t width;
  uint32_t height;
  PixelJitter jitter;
  DepthRange depth;
  uint32_t reset;
  uint32_t use_object_motion;
  uint32_t inverted_depth;
  uint32_t composed_transition = 0;
  std::array<uint32_t, 2> padding{};
  Matrix object_pose_to_previous_clip{};
};

Matrix ApplyProjectionJitter(Matrix projection, PixelJitter jitter, Extent render);
PixelJitter SceneJitter(uint32_t scene_index, Extent render, Extent output);

class CameraHistory {
 public:
  // Call once per scene render for one view using an unjittered row-major matrix.
  // For rebased coordinates, pass the previous projection expressed in the current
  // frame's world space. Repeated presents may pass the same scene_index.
  CameraFrame BeginFrame(uint32_t scene_index, const Matrix& world_to_clip, Extent render,
                         Extent output, bool camera_cut,
                         const std::optional<Matrix>& previous_in_current_world = std::nullopt);

 private:
  bool active_ = false;
  uint32_t last_index_ = 0;
  uint32_t phase_ = 0;
  Extent render_{};
  Extent output_{};
  Matrix last_world_to_clip_{};
  std::optional<Matrix> last_previous_in_current_world_;
  CameraFrame camera_frame_{};
};

class DepthMotionPass {
 public:
  explicit DepthMotionPass(ID3D12Device* device);

  // The SRV table contains single-sample depth, object motion, and a UINT validity mask
  // in that order. The last two may be null SRVs when use_object_motion is zero.
  // UAVs are RG16F or RG32F motion followed by R32F normalized device depth.
  // The caller owns barriers and descriptor lifetime.
  void Record(ID3D12GraphicsCommandList* commands, const DepthMotionConstants& constants,
              ID3D12DescriptorHeap* heap, D3D12_GPU_DESCRIPTOR_HANDLE inputs,
              D3D12_GPU_DESCRIPTOR_HANDLE output) const;

 private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};

}  // namespace asura::temporal
