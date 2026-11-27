#include "temporal_camera.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "depth_motion_bytecode.h"

namespace asura::temporal {
namespace {

bool SameExtent(Extent a, Extent b) {
  return a.width == b.width && a.height == b.height;
}

void CheckExtent(Extent extent) {
  if (!extent.width || !extent.height ||
      extent.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      extent.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
    throw std::invalid_argument("Invalid temporal camera extent");
  }
}

float Halton(uint32_t index, uint32_t base) {
  float value = 0.0f;
  float weight = 1.0f;
  while (index) {
    weight /= static_cast<float>(base);
    value += weight * static_cast<float>(index % base);
    index /= base;
  }
  return value;
}

uint32_t JitterPhaseCount(Extent render, Extent output) {
  const double ratio = std::max({1.0, double(output.width) / render.width,
                                 double(output.height) / render.height});
  return static_cast<uint32_t>(std::ceil(8.0 * ratio * ratio));
}

DirectX::XMMATRIX LoadMatrix(const Matrix& coefficients) {
  DirectX::XMFLOAT4X4 values;
  static_assert(sizeof(values) == sizeof(coefficients));
  std::memcpy(&values, coefficients.data(), sizeof(values));
  return DirectX::XMLoadFloat4x4(&values);
}

Matrix StoreMatrix(DirectX::FXMMATRIX matrix) {
  DirectX::XMFLOAT4X4 values;
  DirectX::XMStoreFloat4x4(&values, matrix);
  Matrix coefficients;
  std::memcpy(coefficients.data(), &values, sizeof(values));
  return coefficients;
}

DirectX::XMMATRIX InvertMatrix(DirectX::FXMMATRIX matrix) {
  const auto inverse = DirectX::XMMatrixInverse(nullptr, matrix);
  if (DirectX::XMMatrixIsNaN(inverse) || DirectX::XMMatrixIsInfinite(inverse)) {
    throw std::invalid_argument("Noninvertible camera view projection");
  }
  return inverse;
}

void Require(HRESULT result, const char* operation) {
  if (FAILED(result)) throw std::runtime_error(operation);
}

constexpr UINT kMotionRootWords = sizeof(DepthMotionConstants) / sizeof(uint32_t);
static_assert(kMotionRootWords == 44);
static_assert(offsetof(DepthMotionConstants, width) == 64);
static_assert(offsetof(DepthMotionConstants, jitter) == 72);
static_assert(offsetof(DepthMotionConstants, depth) == 80);

}  // namespace

Matrix RebasePreviousView(Matrix previous, const ViewOrigin& from, const ViewOrigin& to,
                          float origin_sign) {
  for (size_t col = 0; col < 4; ++col) {
    double translated = previous[12 + col];
    for (size_t axis = 0; axis < 3; ++axis) {
      translated += (double(to[axis]) - from[axis]) * origin_sign * previous[axis * 4 + col];
    }
    previous[12 + col] = static_cast<float>(translated);
  }
  return previous;
}

bool MatchesPreviousXY(const Matrix& previous, const ClipXY& xy) {
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 2; ++col) {
      const float difference = std::abs(previous[row * 4 + col] - xy[row * 2 + col]);
      if (!(difference <= (row == 3 ? 0.125f : 0.00025f))) return false;
    }
  }
  return true;
}

Matrix ApplyProjectionJitter(Matrix projection, PixelJitter jitter, Extent render) {
  CheckExtent(render);
  const float offset_x = 2.0f * jitter.x / render.width;
  const float offset_y = -2.0f * jitter.y / render.height;
  // Applying a homogeneous clip-space translation preserves depth and W.
  for (size_t row = 0; row < 16; row += 4) {
    projection[row] += offset_x * projection[row + 3];
    projection[row + 1] += offset_y * projection[row + 3];
  }
  return projection;
}

PixelJitter SceneJitter(uint32_t scene_index, Extent render, Extent output) {
  CheckExtent(render);
  CheckExtent(output);
  const uint32_t sequence = scene_index % JitterPhaseCount(render, output) + 1;
  return {Halton(sequence, 2) - 0.5f, Halton(sequence, 3) - 0.5f};
}

CameraFrame CameraHistory::BeginFrame(uint32_t scene_index, const Matrix& world_to_clip,
                                      Extent render, Extent output, bool camera_cut,
                                      const std::optional<Matrix>& previous_in_current_world) {
  CheckExtent(render);
  CheckExtent(output);
  const bool extent_changed = !SameExtent(render_, render) || !SameExtent(output_, output);
  if (active_ && !camera_cut && !extent_changed && scene_index == last_index_ &&
      world_to_clip == last_world_to_clip_ &&
      previous_in_current_world == last_previous_in_current_world_) {
    return camera_frame_;
  }

  const auto current = LoadMatrix(world_to_clip);
  const auto current_inverse = InvertMatrix(current);
  const bool reset = !active_ || camera_cut || extent_changed || scene_index != last_index_ + 1;
  const Matrix& previous_projection =
      previous_in_current_world ? *previous_in_current_world : last_world_to_clip_;
  const auto previous = reset ? current : LoadMatrix(previous_projection);
  const auto previous_inverse = reset ? current_inverse : InvertMatrix(previous);
  const uint32_t phase_count = JitterPhaseCount(render, output);
  const uint32_t phase = reset ? 0 : (phase_ + 1) % phase_count;
  camera_frame_ = {
      StoreMatrix(reset ? DirectX::XMMatrixIdentity() : current_inverse * previous),
      StoreMatrix(reset ? DirectX::XMMatrixIdentity() : previous_inverse * current),
      {Halton(phase + 1, 2) - 0.5f, Halton(phase + 1, 3) - 0.5f},
      reset,
  };
  active_ = true;
  last_index_ = scene_index;
  phase_ = phase;
  render_ = render;
  output_ = output;
  last_world_to_clip_ = world_to_clip;
  last_previous_in_current_world_ = previous_in_current_world;
  return camera_frame_;
}

DepthMotionPass::DepthMotionPass(ID3D12Device* device) {
  const D3D12_DESCRIPTOR_RANGE srvs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
  const D3D12_DESCRIPTOR_RANGE uavs{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = kMotionRootWords;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &srvs};
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[2].DescriptorTable = {1, &uavs};
  D3D12_ROOT_SIGNATURE_DESC signature{};
  signature.NumParameters = static_cast<UINT>(std::size(parameters));
  signature.pParameters = parameters;
  Microsoft::WRL::ComPtr<ID3DBlob> serialized;
  Require(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &serialized, nullptr), "Could not serialize motion root signature");
  Require(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(&root_)), "Could not create motion root signature");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = root_.Get();
  pipeline.CS = {asura_depth_motion, sizeof(asura_depth_motion)};
  Require(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_)),
          "Could not create depth motion compute pipeline");
}

void DepthMotionPass::Record(ID3D12GraphicsCommandList* commands,
                             const DepthMotionConstants& constants, ID3D12DescriptorHeap* heap,
                             D3D12_GPU_DESCRIPTOR_HANDLE inputs,
                             D3D12_GPU_DESCRIPTOR_HANDLE output) const {
  CheckExtent({constants.width, constants.height});
  if (!commands || !heap || !inputs.ptr || !output.ptr ||
      !std::isfinite(constants.depth.min) || !std::isfinite(constants.depth.max) ||
      constants.depth.min < 0.0f || constants.depth.max > 1.0f ||
      constants.depth.min >= constants.depth.max) {
    throw std::invalid_argument("Invalid depth motion pass inputs");
  }
  commands->SetDescriptorHeaps(1, &heap);
  commands->SetPipelineState(pipeline_.Get());
  commands->SetComputeRootSignature(root_.Get());
  commands->SetComputeRoot32BitConstants(0, kMotionRootWords, &constants, 0);
  commands->SetComputeRootDescriptorTable(1, inputs);
  commands->SetComputeRootDescriptorTable(2, output);
  commands->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);
}

}  // namespace asura::temporal
