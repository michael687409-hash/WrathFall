#pragma once

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace asura::temporal {

enum class Algorithm { Dlaa, Dlss, Fsr3, Fsr4 };
enum class Quality { Native, Quality, Balanced, Performance, UltraPerformance };
enum class DlssPreset { Default, J, K, L, M };

struct Extent {
  uint32_t width;
  uint32_t height;
};

struct Settings {
  Algorithm algorithm;
  Quality quality;
  DlssPreset preset;
  Extent output;
  bool hdr;
  bool inverted_depth;
  // When supplied, the title's internal render extent takes precedence over
  // the vendor quality mode's suggested extent.
  std::optional<Extent> fixed_render;
};

struct Camera {
  // Row-major matrices without projection jitter.
  std::array<float, 16> view_to_clip;
  std::array<float, 16> clip_to_view;
  std::array<float, 16> clip_to_previous_clip;
  std::array<float, 16> previous_clip_to_clip;
  std::array<float, 3> position;
  std::array<float, 3> up;
  std::array<float, 3> right;
  std::array<float, 3> forward;
  float near_plane;
  float far_plane;
  float vertical_fov;
  float view_space_to_meters;
};

struct Frame {
  // HUD-free color, single-sample depth, and dense current-to-previous XY displacement
  // in render pixels, with +Y down and with projection jitter removed.
  ID3D12Resource* color;
  ID3D12Resource* depth;
  ID3D12Resource* motion;
  ID3D12Resource* output;
  ID3D12Resource* reactive;
  Extent render;
  Camera camera;
  uint32_t index;
  float jitter_x;
  float jitter_y;
  float elapsed_ms;
  bool reset_history;
};

class Upscaler {
 public:
  // For Streamline, construct before creating the D3D12 device. The same instance must own
  // the process-wide Streamline session. The DLL path is the trusted SDK runtime directory.
  Upscaler(Settings settings, const std::filesystem::path& runtime_directory);
  ~Upscaler();
  Upscaler(const Upscaler&) = delete;
  Upscaler& operator=(const Upscaler&) = delete;

  // Returns the fixed render extent or the selected vendor mode's extent.
  Extent Initialize(ID3D12Device* device);

  // Inputs must be NON_PIXEL_SHADER_RESOURCE and output UNORDERED_ACCESS. Dispatch is
  // recorded into the caller's command list. Rebind pipeline state afterwards and retire
  // GPU work before destroying the upscaler or the referenced resources.
  void Dispatch(ID3D12GraphicsCommandList* commands, const Frame& frame);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace asura::temporal
