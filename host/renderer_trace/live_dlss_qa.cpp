#include "live_dlss_qa.h"
#include "../upscaler_events.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <vector>

#include "DXBCChecksum.h"
#include "copy_transition_bytecode.h"
#include "fsr3_reactive_bytecode.h"
#include "object_pose_bytecode.h"
#include "prepare_hud_target_bytecode.h"

namespace asura::temporal::live {
namespace {
namespace events = asura::upscaler_events;
using Microsoft::WRL::ComPtr;
using namespace DirectX;
constexpr Extent kRender{1280, 720};
constexpr Extent kOutput{2560, 1440};
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kCopy = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto kPoseTarget = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr auto kNone = std::numeric_limits<uint64_t>::max();
constexpr auto kPoseFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr auto kLoadingCardMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
    2, 1, 0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
struct PoseProgram {
  uint64_t vertex;
  uint64_t pixel;
  uint32_t original_register;
  uint32_t previous_register;
  bool full_output;
};

constexpr PoseProgram kPosePrograms[] = {
    {0x3f0f85e037e44989, 0xa9e9e8cbe344da0d, 20, 3, false},
    {0x554d193f769e1c8d, 0xa9e9e8cbe344da0d, 19, 5, false},
    {0xa08ba98b85f6389e, 0xa9e9e8cbe344da0d, 19, 3, false},
    {0xe88fbd7c84daf47d, 0xa9e9e8cbe344da0d, 19, 5, false},
    {0x6dc5768828c67824, 0xa9e9e8cbe344da0d, 19, 5, false},
    {0xfdf05723ac4c1710, 0xa9e9e8cbe344da0d, 19, 5, false},
    {0x68c798b21d1e8771, 0xa9e9e8cbe344da0d, 19, 5, false},
    {0x457ae6d64000e6f9, 0xa9e9e8cbe344da0d, 19, 5, false},
    {0x94409641c1c6325d, 0xcfc6d41016f07ac2, 8, 4, true},
    {0xb1ea17ef03a47b77, 0xcfc6d41016f07ac2, 19, 5, true},
    {0x37a5891df3cb3374, 0xcfc6d41016f07ac2, 19, 5, true},
    {0x25cc0047165e5d2a, 0xcfc6d41016f07ac2, 19, 5, true},
    {0x8d1476b567f7d4ea, 0xcfc6d41016f07ac2, 19, 5, true},
    {0x4894f3b6f88e018c, 0xcfc6d41016f07ac2, 8, 4, true},
};

std::wstring Env(const wchar_t* name) {
  const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
  if (!length) return {};
  std::wstring value(length, L'\0');
  const DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
  if (copied >= length) return {};
  value.resize(copied);
  return value;
}

std::vector<uint8_t> PriorPoseShader(std::span<const uint8_t> vertex, const PoseProgram& program) {
  if (vertex.size() < 36 || std::memcmp(vertex.data(), "DXBC", 4)) {
    throw std::runtime_error("missing translated object pose shader");
  }
  std::vector<uint8_t> shader(vertex.begin(), vertex.end());
  const auto word_at = [&](size_t offset) {
    uint32_t word;
    std::memcpy(&word, shader.data() + offset, sizeof(word));
    return word;
  };
  const uint32_t chunks = word_at(28);
  if (size_t(chunks) > (shader.size() - 32) / 4) {
    throw std::runtime_error("invalid object pose shader header");
  }
  unsigned changes = 0;
  for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
    const size_t start = word_at(32 + chunk * 4);
    if (start > shader.size() - 8) throw std::runtime_error("invalid object pose shader section");
    if (std::memcmp(shader.data() + start, "SHEX", 4)) continue;
    const size_t end = start + 8 + word_at(start + 4);
    if (end > shader.size()) throw std::runtime_error("invalid object pose shader size");
    for (size_t pos = start + 16; pos + 4 <= end;) {
      const auto token = word_at(pos);
      const unsigned opcode = token & 0x7ff;
      const uint32_t size =
          opcode == 53 && pos + 8 <= end ? word_at(pos + 4) : (token >> 24) & 127;
      if (!size || pos + size_t(size) * 4 > end) {
        throw std::runtime_error("invalid object pose shader instruction");
      }
      if (opcode == 54 && size == 5 &&
          word_at(pos + 4) == (program.full_output ? 0x1020f2u : 0x102032u) &&
          word_at(pos + 8) == 0 &&
          word_at(pos + 12) == (program.full_output ? 0x100e46u : 0x100046u) &&
          word_at(pos + 16) == program.original_register) {
        // Export the homogeneous prior-pose position through TEXCOORD0.
        const uint32_t dest = 0x1020f2;
        const uint32_t source[] = {0x100e46, program.previous_register};
        std::memcpy(shader.data() + pos + 4, &dest, sizeof(dest));
        std::memcpy(shader.data() + pos + 12, source, sizeof(source));
        ++changes;
      }
      pos += size * 4;
    }
  }
  if (changes != 1) throw std::runtime_error("expected one prior pose export");
  uint32_t checksum[4]{};
  CalculateDXBCChecksum(shader.data(), static_cast<unsigned>(shader.size()), checksum);
  std::memcpy(shader.data() + 4, checksum, sizeof(checksum));
  return shader;
}

void Require(HRESULT status, const char* operation) {
  if (FAILED(status)) throw std::runtime_error(operation);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_DESC Texture(Extent extent, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
  return {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, extent.width, UINT16(extent.height),
          1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
}

ComPtr<ID3D12Resource> Allocate(ID3D12Device* device, D3D12_RESOURCE_DESC description,
                               D3D12_RESOURCE_STATES initial,
                               D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT) {
  const D3D12_HEAP_PROPERTIES heap{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                   D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  ComPtr<ID3D12Resource> resource;
  Require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                          initial, nullptr, IID_PPV_ARGS(&resource)),
          "allocating temporal input or output");
  return resource;
}

Matrix HostClip(Matrix guest, const float* scale, const float* offset) {
  for (size_t row = 0; row < 16; row += 4) {
    for (size_t axis = 0; axis < 3; ++axis) {
      guest[row + axis] = guest[row + axis] * scale[axis] + guest[row + 3] * offset[axis];
    }
  }
  return guest;
}

enum class CameraSource { CurrentOnly, Rebased, Native };

struct CameraObservation {
  uint64_t frame = kNone;
  Matrix projection{};
  std::optional<ViewOrigin> origin;
};

struct OpaqueView {
  Matrix projection;
  std::array<float, 3> scale, offset;
  uint64_t shader;
  uint32_t first_register;
  uint64_t corroborating_shader = 0;
};

bool IsRebasedSceneProjection(const Matrix& value) {
  if (!std::all_of(value.begin(), value.end(), [](float v) { return std::isfinite(v); })) {
    return false;
  }
  const auto x = XMVectorSet(value[0], value[4], value[8], 0);
  const auto y = XMVectorSet(value[1], value[5], value[9], 0);
  const auto z = XMVectorSet(value[2], value[6], value[10], 0);
  const auto w = XMVectorSet(value[3], value[7], value[11], 0);
  const auto dot = [](FXMVECTOR a, FXMVECTOR b) { return XMVectorGetX(XMVector3Dot(a, b)); };
  const auto xx = dot(x, x);
  const auto yy = dot(y, y);
  const auto zw = dot(z, w);
  const auto z_error = z - w * zw;
  // The primary perspective view has a unit forward column, orthogonal screen
  // axes, a 16:9 aspect, and the title's 10 cm near plane in rebased coordinates.
  return std::abs(dot(w, w) - 1.0f) < 0.001f && xx > 0.25f && xx < 625.0f &&
         std::abs(yy / xx - (16.0f / 9.0f) * (16.0f / 9.0f)) < 0.005f &&
         std::abs(dot(x, y)) < 0.002f && std::abs(dot(x, w)) < 0.002f &&
         std::abs(dot(y, w)) < 0.002f && std::abs(zw - 0.999f) < 0.001f &&
         dot(z_error, z_error) < 0.000001f && std::abs(value[14] - value[15] * zw + 9.99f) < 0.08f &&
         std::abs(value[12]) < 0.08f && std::abs(value[13]) < 0.08f && std::abs(value[15]) < 0.08f;
}

bool MatchesNativeCamera(const Matrix& rebased, const Matrix& native) {
  for (size_t i = 0; i < native.size(); ++i) {
    if (!(std::abs(rebased[i] - native[i]) <= (i >= 12 ? 0.125f : 0.00025f))) {
      return false;
    }
  }
  return true;
}

template <size_t Size>
void LogFloats(std::ostream& log, const std::array<float, Size>& values) {
  for (const auto value : values) log << '\t' << value;
}

XMMATRIX Load(const Matrix& data) {
  return XMMATRIX(data.data());
}

Matrix Store(FXMMATRIX matrix) {
  XMFLOAT4X4 stored;
  XMStoreFloat4x4(&stored, matrix);
  Matrix data;
  std::memcpy(data.data(), &stored, sizeof(stored));
  return data;
}

Camera DescribeCamera(const Matrix& clip, const CameraFrame& frame) {
  const auto x = XMVectorSet(clip[0], clip[4], clip[8], 0);
  const auto y = XMVectorSet(clip[1], clip[5], clip[9], 0);
  const auto z = XMVectorSet(clip[2], clip[6], clip[10], 0);
  const auto w = XMVectorSet(clip[3], clip[7], clip[11], 0);
  const float w_scale = XMVectorGetX(XMVector3Length(w));
  if (w_scale < 1e-6f) throw std::runtime_error("unrecognized perspective camera");
  const auto forward = XMVectorScale(w, 1.0f / w_scale);
  const float offset_x = XMVectorGetX(XMVector3Dot(x, forward));
  const float offset_y = XMVectorGetX(XMVector3Dot(y, forward));
  const auto right = x - forward * offset_x;
  const auto up = y - forward * offset_y;
  const auto position = XMVector4Transform(XMVectorSet(0, 0, 1, 0),
                                         XMMatrixInverse(nullptr, Load(clip)));
  const float inv_position_w = 1.0f / XMVectorGetW(position);
  const float scale_x = XMVectorGetX(XMVector3Length(right)) / w_scale;
  const float scale_y = XMVectorGetX(XMVector3Length(up)) / w_scale;
  const float scale_z = XMVectorGetX(XMVector3Dot(z, forward)) / w_scale;
  const float offset_z = (clip[14] - clip[15] * scale_z) / w_scale;
  Camera camera{};
  camera.view_to_clip = {scale_x, 0, 0, 0, 0, scale_y, 0, 0,
                         offset_x / w_scale, offset_y / w_scale, scale_z, 1,
                         0, 0, offset_z, 0};
  camera.clip_to_view = Store(XMMatrixInverse(nullptr, Load(camera.view_to_clip)));
  camera.clip_to_previous_clip = frame.clip_to_previous_clip;
  camera.previous_clip_to_clip = frame.previous_clip_to_clip;
  const auto vector = [](FXMVECTOR value) {
    return std::array<float, 3>{XMVectorGetX(value), XMVectorGetY(value), XMVectorGetZ(value)};
  };
  camera.position = vector(position * inv_position_w);
  camera.right = vector(XMVector3Normalize(right));
  camera.up = vector(XMVector3Normalize(up));
  camera.forward = vector(forward);
  camera.near_plane = offset_z / (1.0f - scale_z);
  camera.far_plane = scale_z < -1.0e-7f ? -offset_z / scale_z : 1.0e7f;
  camera.vertical_fov = 2.0f * std::atan(1.0f / scale_y);
  camera.view_space_to_meters = 0.01f;
  return camera;
}

void SaveBitmap(const std::filesystem::path& path, const std::vector<uint8_t>& pixels) {
  constexpr DWORD offset = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  const BITMAPFILEHEADER header{0x4D42, offset + static_cast<DWORD>(pixels.size()), 0, 0, offset};
  BITMAPINFOHEADER info{};
  info.biSize = sizeof(info);
  info.biWidth = kOutput.width;
  info.biHeight = -LONG(kOutput.height);
  info.biPlanes = 1;
  info.biBitCount = 32;
  std::ofstream image(path, std::ios::binary);
  image.write(reinterpret_cast<const char*>(&header), sizeof(header));
  image.write(reinterpret_cast<const char*>(&info), sizeof(info));
  image.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
  if (!image) throw std::runtime_error("writing temporal output");
}

struct State {
  State(const std::filesystem::path& directory, const std::wstring& runtime, Algorithm algorithm)
      : directory(directory), log(directory / "live-temporal.tsv", std::ios::trunc),
        provider(algorithm == Algorithm::Fsr3 ? "fsr3" : "dlss"),
        present(Env(L"ASURA_LIVE_DLSS_PRESENT") == L"1"),
        diagnostics(Env(L"ASURA_LIVE_DLSS_DIAGNOSTICS") != L"0"),
        trace_transitions(Env(L"ASURA_LIVE_DLSS_TRANSITION_TRACE") == L"1"),
        reactive_enabled(algorithm == Algorithm::Fsr3 && Env(L"ASURA_FSR3_REACTIVE") != L"0") {
    if (!log) throw std::runtime_error("could not open temporal QA log");
    Settings settings{algorithm, Quality::Performance, DlssPreset::Default,
                      kOutput, false, true, kRender};
    log << "prepared\t1280x720\t2560x1440\tprovider=" << provider;
    if (algorithm == Algorithm::Dlss) {
      static constexpr std::pair<std::wstring_view, DlssPreset> presets[]{
          {L"Default", DlssPreset::Default}, {L"J", DlssPreset::J}, {L"K", DlssPreset::K},
          {L"L", DlssPreset::L}, {L"M", DlssPreset::M}};
      const auto requested_preset = Env(L"ASURA_DLSS_PRESET");
      const std::wstring_view preset_name =
          requested_preset.empty() ? std::wstring_view(L"K") : std::wstring_view(requested_preset);
      const auto selected = std::find_if(std::begin(presets), std::end(presets),
          [preset_name](const auto& entry) { return entry.first == preset_name; });
      if (selected == std::end(presets)) throw std::runtime_error("Unsupported DLSS model preset");
      settings.preset = selected->second;
      log << "\tpreset=" << std::string(preset_name.begin(), preset_name.end());
    }
    upscaler.emplace(settings, runtime);
    log << "\treactive_mask=" << reactive_enabled << std::endl;
  }

  void InitializeHud(ID3D12Device* device) {
    constexpr auto flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hud = Allocate(device, Texture(kOutput, DXGI_FORMAT_R8G8B8A8_UNORM, flags), kPoseTarget);
    const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    Require(device->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&hud_rtv)),
            "allocating the HUD render target view");
    device->CreateRenderTargetView(hud.Get(), nullptr, hud_rtv->GetCPUDescriptorHandleForHeapStart());
    const D3D12_DESCRIPTOR_HEAP_DESC shader_heap{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Require(device->CreateDescriptorHeap(&shader_heap, IID_PPV_ARGS(&hud_heap)),
            "allocating HUD composition descriptors");
    auto descriptor = hud_heap->GetCPUDescriptorHandleForHeapStart();
    const auto stride = device->GetDescriptorHandleIncrementSize(shader_heap.Type);
    device->CreateShaderResourceView(output.Get(), nullptr, descriptor);
    descriptor.ptr += stride;
    device->CreateShaderResourceView(color.Get(), nullptr, descriptor);
    descriptor.ptr += stride;
    device->CreateUnorderedAccessView(hud.Get(), nullptr, nullptr, descriptor);

    const D3D12_DESCRIPTOR_RANGE ranges[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2},
    };
    D3D12_ROOT_PARAMETER table{};
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable = {2, ranges};
    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_SIGNATURE_DESC signature{1, &table};
    ComPtr<ID3DBlob> encoded;
    Require(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                       &encoded, nullptr), "encoding the HUD root signature");
    Require(device->CreateRootSignature(0, encoded->GetBufferPointer(), encoded->GetBufferSize(),
                                        IID_PPV_ARGS(&hud_root)), "creating the HUD root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = hud_root.Get();
    pipeline.CS = {asura_prepare_hud_target, sizeof(asura_prepare_hud_target)};
    Require(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&hud_prepare)),
            "creating the HUD preparation pipeline");

    transition_raw = Allocate(device, Texture(kRender, DXGI_FORMAT_R8G8B8A8_UNORM,
                                               D3D12_RESOURCE_FLAG_NONE), kCopy);
    Require(device->CreateDescriptorHeap(&shader_heap, IID_PPV_ARGS(&transition_heap)),
            "allocating loading card descriptors");
    descriptor = transition_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC null_view{};
    null_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &null_view, descriptor);
    descriptor.ptr += stride;
    device->CreateShaderResourceView(transition_raw.Get(), nullptr, descriptor);
    descriptor.ptr += stride;
    device->CreateUnorderedAccessView(color.Get(), nullptr, nullptr, descriptor);
    pipeline.CS = {asura_copy_transition, sizeof(asura_copy_transition)};
    Require(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&transition_copy)),
            "creating the loading card conversion pipeline");

    uint64_t bytes = 0;
    for (const auto& [texture, footprint] :
         {std::pair{hud.Get(), &hud_layout}, std::pair{output.Get(), &hud_scene_layout}}) {
      const auto desc = texture->GetDesc();
      const auto offset = (bytes + 511) & ~UINT64(511);
      device->GetCopyableFootprints(&desc, 0, 1, offset, footprint, nullptr, nullptr, nullptr);
      bytes = footprint->Offset + UINT64(footprint->Footprint.RowPitch) * desc.Height;
    }
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1,
                                    DXGI_FORMAT_UNKNOWN, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    hud_sample = Allocate(device, buffer, kCopy, D3D12_HEAP_TYPE_READBACK);
  }

  void InitializeReactivity(ID3D12Device* device) {
    prior_color = Allocate(device, Texture(kRender, DXGI_FORMAT_R8G8B8A8_UNORM,
                                           D3D12_RESOURCE_FLAG_NONE), kRead);
    reactive_mask = Allocate(device, Texture(kRender, DXGI_FORMAT_R8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), kWrite);
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Require(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&reactive_heap)),
            "allocating reactive-mask descriptors");
    auto cpu = reactive_heap->GetCPUDescriptorHandleForHeapStart();
    const auto stride = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    for (auto* texture : {color.Get(), prior_color.Get(), motion.Get()}) {
      device->CreateShaderResourceView(texture, nullptr, cpu);
      cpu.ptr += stride;
    }
    device->CreateUnorderedAccessView(reactive_mask.Get(), nullptr, nullptr, cpu);

    const D3D12_DESCRIPTOR_RANGE ranges[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3},
    };
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 4};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    const D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 1, &sampler};
    ComPtr<ID3DBlob> encoded;
    Require(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                       &encoded, nullptr), "encoding the reactive-mask root signature");
    Require(device->CreateRootSignature(0, encoded->GetBufferPointer(), encoded->GetBufferSize(),
        IID_PPV_ARGS(&reactive_root)), "creating the reactive-mask root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = reactive_root.Get();
    pipeline.CS = {asura_fsr3_reactive, sizeof(asura_fsr3_reactive)};
    Require(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&reactive_pipeline)),
            "creating the reactive-mask pipeline");
  }

  void Initialize(ID3D12Device* device, ID3D12Resource* source) {
    upscaler->Initialize(device);
    motion_pass.emplace(device);
    raw_depth = Allocate(device, source->GetDesc(), kCopy);
    constexpr auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    color = Allocate(device, Texture(kRender, DXGI_FORMAT_R8G8B8A8_UNORM, uav), kCopy);
    motion = Allocate(device, Texture(kRender, DXGI_FORMAT_R16G16_FLOAT, uav), kWrite);
    depth = Allocate(device, Texture(kRender, DXGI_FORMAT_R32_FLOAT, uav), kWrite);
    output = Allocate(device, Texture(kOutput, DXGI_FORMAT_R16G16B16A16_FLOAT, uav), kWrite);
    if (present) InitializeHud(device);
    if (reactive_enabled) InitializeReactivity(device);
    pose = Allocate(device, Texture(kRender, kPoseFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                    kPoseTarget);
    D3D12_DESCRIPTOR_HEAP_DESC pose_descriptor{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    Require(device->CreateDescriptorHeap(&pose_descriptor, IID_PPV_ARGS(&pose_rtv)),
            "allocating pose render target view");
    device->CreateRenderTargetView(pose.Get(), nullptr, pose_rtv->GetCPUDescriptorHandleForHeapStart());
    pose_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    Require(device->CreateDescriptorHeap(&pose_descriptor, IID_PPV_ARGS(&pose_dsv)),
            "allocating pose depth view");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    device->CreateDepthStencilView(raw_depth.Get(), &dsv, pose_dsv->GetCPUDescriptorHandleForHeapStart());
    const D3D12_DESCRIPTOR_HEAP_DESC descriptor{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Require(device->CreateDescriptorHeap(&descriptor, IID_PPV_ARGS(&heap)),
            "allocating temporal descriptors");
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    const auto stride = device->GetDescriptorHandleIncrementSize(descriptor.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(raw_depth.Get(), &srv, cpu);
    cpu.ptr += stride;
    srv.Format = kPoseFormat;
    device->CreateShaderResourceView(pose.Get(), &srv, cpu);
    cpu.ptr += stride;
    srv.Format = DXGI_FORMAT_R32_UINT;
    device->CreateShaderResourceView(nullptr, &srv, cpu);
    cpu.ptr += stride;
    device->CreateUnorderedAccessView(motion.Get(), nullptr, nullptr, cpu);
    cpu.ptr += stride;
    device->CreateUnorderedAccessView(depth.Get(), nullptr, nullptr, cpu);
    uav_table = {heap->GetGPUDescriptorHandleForHeapStart().ptr + stride * 3};

    UINT64 bytes = 0;
    for (const auto& [texture, footprint] : {
             std::pair{output.Get(), &layout}, std::pair{pose.Get(), &pose_layout},
             std::pair{motion.Get(), &motion_layout}, std::pair{depth.Get(), &depth_layout},
             std::pair{reactive_mask.Get(), &reactive_layout}}) {
      if (!texture) continue;
      const auto desc = texture->GetDesc();
      const auto offset = (bytes + 511) & ~UINT64(511);
      device->GetCopyableFootprints(&desc, 0, 1, offset, footprint, nullptr, nullptr, nullptr);
      bytes = footprint->Offset + UINT64(footprint->Footprint.RowPitch) * desc.Height;
    }
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1,
                                    DXGI_FORMAT_UNKNOWN, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    sample = Allocate(device, buffer, kCopy, D3D12_HEAP_TYPE_READBACK);
  }

  void PrepareTransitionColor(ID3D12GraphicsCommandList* list) {
    Transition(list, transition_raw.Get(), kCopy, kRead);
    Transition(list, color.Get(), kCopy, kWrite);
    auto* descriptors = transition_heap.Get();
    list->SetDescriptorHeaps(1, &descriptors);
    list->SetComputeRootSignature(hud_root.Get());
    list->SetPipelineState(transition_copy.Get());
    list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((kRender.width + 7) / 8, (kRender.height + 7) / 8, 1);
    Transition(list, color.Get(), kWrite, kRead);
    Transition(list, transition_raw.Get(), kRead, kCopy);
  }

  ID3D12Resource* PrepareReactiveMask(ID3D12GraphicsCommandList* list, uint64_t frame,
                                     PixelJitter jitter, bool reset) {
    if (!reactive_enabled || reset || prior_color_frame == kNone || prior_color_frame + 1 != frame) {
      return nullptr;
    }
    const struct {
      uint32_t width, height;
      PixelJitter jitter_delta;
    } constants{kRender.width, kRender.height,
                {prior_color_jitter.x - jitter.x, prior_color_jitter.y - jitter.y}};
    auto* descriptors = reactive_heap.Get();
    list->SetDescriptorHeaps(1, &descriptors);
    list->SetComputeRootSignature(reactive_root.Get());
    list->SetPipelineState(reactive_pipeline.Get());
    list->SetComputeRoot32BitConstants(0, 4, &constants, 0);
    list->SetComputeRootDescriptorTable(1, descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((kRender.width + 7) / 8, (kRender.height + 7) / 8, 1);
    Transition(list, reactive_mask.Get(), kWrite, kRead);
    return reactive_mask.Get();
  }

  void ReleaseSceneColor(ID3D12GraphicsCommandList* list, uint64_t frame,
                         PixelJitter jitter, bool composed) {
    if (!reactive_enabled || composed) {
      Transition(list, color.Get(), kRead, kCopy);
      if (composed) prior_color_frame = kNone;
      return;
    }
    constexpr auto copy_source = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Transition(list, color.Get(), kRead, copy_source);
    Transition(list, prior_color.Get(), kRead, kCopy);
    list->CopyResource(prior_color.Get(), color.Get());
    Transition(list, prior_color.Get(), kCopy, kRead);
    Transition(list, color.Get(), copy_source, kCopy);
    prior_color_frame = frame;
    prior_color_jitter = jitter;
  }

  void PrepareHud(ID3D12GraphicsCommandList* list, uint64_t frame) {
    Transition(list, output.Get(), kWrite, kRead);
    Transition(list, hud.Get(), kPoseTarget, kWrite);
    auto* descriptors = hud_heap.Get();
    list->SetDescriptorHeaps(1, &descriptors);
    list->SetComputeRootSignature(hud_root.Get());
    list->SetPipelineState(hud_prepare.Get());
    list->SetComputeRootDescriptorTable(0, hud_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((kOutput.width + 7) / 8, (kOutput.height + 7) / 8, 1);
    Transition(list, hud.Get(), kWrite, kPoseTarget);
    Transition(list, output.Get(), kRead, kWrite);
    hud_frame = frame;
    hud_draws = 0;
    hud_sealed = false;
  }

  void RecordHudSample(ID3D12GraphicsCommandList* list, uint64_t submission, uint64_t completed) {
    if (hud_sample_submission != kNone && completed >= hud_sample_submission) {
      void* mapped = nullptr;
      Require(hud_sample->Map(0, nullptr, &mapped), "reading the composed HUD");
      const auto* buffer = static_cast<const uint8_t*>(mapped);
      std::vector<uint8_t> pixels(size_t(kOutput.width) * kOutput.height * 4);
      size_t modified = 0;
      for (uint32_t y = 0; y < kOutput.height; ++y) {
        const auto* composite = buffer + hud_layout.Offset + size_t(y) * hud_layout.Footprint.RowPitch;
        const auto* scene = reinterpret_cast<const uint16_t*>(
            buffer + hud_scene_layout.Offset + size_t(y) * hud_scene_layout.Footprint.RowPitch);
        for (uint32_t x = 0; x < kOutput.width; ++x) {
          const size_t position = (size_t(y) * kOutput.width + x) * 4;
          bool changed = false;
          for (uint32_t c = 0; c < 3; ++c) {
            const float value = PackedVector::XMConvertHalfToFloat(scene[4 * x + c]);
            const float baseline = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
            const uint8_t actual = composite[4 * x + c];
            changed |= std::abs(float(actual) - baseline * 255.0f) > 1.0f;
            pixels[position + 2 - c] = actual;
          }
          pixels[position + 3] = 255;
          modified += changed;
        }
      }
      const D3D12_RANGE no_write{0, 0};
      hud_sample->Unmap(0, &no_write);
      SaveBitmap(directory / ("composite-" + std::to_string(hud_sample_frame) + ".bmp"), pixels);
      log << "hud-sample\t" << hud_sample_frame << "\tchanged_pixels=" << modified << std::endl;
      hud_sample_submission = kNone;
    }
    if (hud_to_sample == kNone || hud_sample_submission != kNone) return;
    const auto copy = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                          const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout) {
      constexpr auto source_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      Transition(list, resource, before, source_state);
      const D3D12_TEXTURE_COPY_LOCATION source{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
      D3D12_TEXTURE_COPY_LOCATION dest{hud_sample.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dest.PlacedFootprint = layout;
      list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
      Transition(list, resource, source_state, before);
    };
    copy(hud.Get(), kPoseTarget, hud_layout);
    copy(output.Get(), kWrite, hud_scene_layout);
    hud_sample_frame = hud_to_sample;
    hud_to_sample = kNone;
    hud_sample_submission = submission;
    ++hud_samples;
  }

  ID3D12PipelineState* ObjectPipeline(ID3D12Device* device, ID3D12RootSignature* root,
                                     std::span<const uint8_t> vertex, const PoseProgram& program) {
    auto result = std::find_if(pipelines.begin(), pipelines.end(), [&](const auto& item) {
      return item.root.Get() == root && item.vertex == program.vertex;
    });
    if (result != pipelines.end()) return result->pipeline.Get();
    PosePipeline& entry = pipelines.emplace_back(PosePipeline{root, program.vertex, {}});
    const auto shader = PriorPoseShader(vertex, program);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {shader.data(), shader.size()};
    desc.PS = {asura_object_pose, sizeof(asura_object_pose)};
    auto& blend = desc.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = kPoseFormat;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    desc.SampleDesc.Count = 1;
    const auto status = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&entry.pipeline));
    log << "pose-pipeline\t" << std::hex << program.vertex << "\tstatus=" << status << std::dec
        << std::endl;
    return entry.pipeline.Get();
  }

  void SaveSample() {
    void* mapped = nullptr;
    Require(sample->Map(0, nullptr, &mapped), "reading temporal output");
    const auto* buffer = static_cast<const uint8_t*>(mapped);
    const auto* base = buffer + layout.Offset;
    std::vector<uint8_t> pixels(size_t(kOutput.width) * kOutput.height * 4);
    float min_luminance = 1.f, max_luminance = 0.f;
    size_t invalid_color = 0;
    for (uint32_t y = 0; y < kOutput.height; ++y) {
      const auto* row = reinterpret_cast<const uint16_t*>(base + size_t(y) * layout.Footprint.RowPitch);
      for (uint32_t x = 0; x < kOutput.width; ++x) {
        const auto location = (size_t(y) * kOutput.width + x) * 4;
        float luminance = 0.f;
        for (int channel = 0; channel < 3; ++channel) {
          const float value = PackedVector::XMConvertHalfToFloat(row[4 * x + channel]);
          invalid_color += !std::isfinite(value);
          const float c = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
          pixels[location + 2 - channel] = uint8_t(c * 255.0f + 0.5f);
          luminance += c / 3;
        }
        min_luminance = std::min(min_luminance, luminance);
        max_luminance = std::max(max_luminance, luminance);
        pixels[location + 3] = 255;
      }
    }
    size_t pose_pixels = 0, independent_pixels = 0, nonzero_motion = 0, reactive_pixels = 0;
    const auto to_previous = Load(sample_history.clip_to_previous_clip);
    for (uint32_t y = 0; y < kRender.height; ++y) {
      const auto row = [buffer, y](const auto& slice) {
        return buffer + slice.Offset + size_t(y) * slice.Footprint.RowPitch;
      };
      const auto* vectors = reinterpret_cast<const uint16_t*>(row(motion_layout));
      const auto* poses = reinterpret_cast<const float*>(row(pose_layout));
      const auto* depths = reinterpret_cast<const float*>(row(depth_layout));
      const auto* reactive = sample_has_reactive ? row(reactive_layout) : nullptr;
      for (uint32_t x = 0; x < kRender.width; ++x) {
        reactive_pixels += reactive && reactive[x] >= 26;
        const float dx = PackedVector::XMConvertHalfToFloat(vectors[2 * x]);
        const float dy = PackedVector::XMConvertHalfToFloat(vectors[2 * x + 1]);
        nonzero_motion += dx != 0 || dy != 0;
        if (!sample_has_pose || !std::isfinite(poses[4 * x + 3]) || poses[4 * x + 3] <= 1.e-6f) {
          continue;
        }
        ++pose_pixels;
        if (sample_history.reset) continue;
        const float clip_x = (float(x) + 0.5f - sample_jitter.x) * (2.f / kRender.width) - 1.f;
        const float clip_y = (float(y) + 0.5f - sample_jitter.y) * (-2.f / kRender.height) + 1.f;
        const auto previous = XMVector4Transform(XMVectorSet(clip_x, clip_y, depths[x], 1.f),
                                                 to_previous);
        const float w = XMVectorGetW(previous);
        if (w <= 1.e-6f) continue;
        const float static_x = (XMVectorGetX(previous) / w - clip_x) * (kRender.width * .5f);
        const float static_y = (XMVectorGetY(previous) / w - clip_y) * (kRender.height * -.5f);
        independent_pixels += std::hypot(dx - static_x, dy - static_y) > 0.5f;
      }
    }
    const D3D12_RANGE no_write{0, 0};
    sample->Unmap(0, &no_write);
    SaveBitmap(directory / (std::string(provider) + '-' + std::to_string(sample_frame) + ".bmp"),
               pixels);
    log << "sample\t" << sample_frame << "\tcolor_range=" << min_luminance << ',' << max_luminance
        << "\tinvalid_channels=" << invalid_color << "\tpose_pixels=" << pose_pixels
        << "\tindependent_pixels=" << independent_pixels << "\tnonzero_motion=" << nonzero_motion
        << "\treactive_pixels=" << reactive_pixels
        << std::endl;
  }

  std::filesystem::path directory;
  std::ofstream log;
  const std::string_view provider;
  std::optional<Upscaler> upscaler;
  std::optional<DepthMotionPass> motion_pass;
  CameraHistory history;
  Matrix current{}, previous{}, last_camera{}, host_basis{};
  ComPtr<ID3D12Resource> raw_depth, color, motion, depth, output, sample, pose;
  ComPtr<ID3D12DescriptorHeap> heap, pose_rtv, pose_dsv;
  struct PosePipeline {
    ComPtr<ID3D12RootSignature> root;
    uint64_t vertex;
    ComPtr<ID3D12PipelineState> pipeline;
  };
  std::vector<PosePipeline> pipelines;
  D3D12_GPU_DESCRIPTOR_HANDLE uav_table{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT pose_layout{}, motion_layout{}, depth_layout{}, reactive_layout{};
  CameraFrame sample_history{};
  PixelJitter sample_jitter{};
  bool sample_has_pose = false;
  bool sample_has_reactive = false;
  uint64_t camera_frame = kNone, depth_frame = kNone, color_frame = kNone;
  CameraObservation camera_view, prior_camera_view;
  uint64_t opaque_frame = kNone;
  std::array<OpaqueView, 8> opaque_views{};
  size_t opaque_view_count = 0;
  std::set<std::pair<uint64_t, uint32_t>> verified_opaque_layouts;
  bool opaque_view_recovered = false;
  CameraSource camera_source = CameraSource::CurrentOnly;
  int origin_checks = 0;
  unsigned camera_audit_samples = 0;
  bool motion_camera_observed = false;
  uint64_t sample_frame = kNone, sample_submission = kNone, last_matched = kNone;
  uint64_t pose_frame = kNone;
  unsigned samples = 0, pose_draws = 0;
  std::set<std::string> candidates;
  std::set<std::array<uint64_t, 4>> observed_depth_surfaces;
  bool enabled = true, pending = false;
  const bool present;
  const bool diagnostics;
  const bool trace_transitions;
  const bool reactive_enabled;
  ComPtr<ID3D12Resource> prior_color, reactive_mask;
  ComPtr<ID3D12DescriptorHeap> reactive_heap;
  ComPtr<ID3D12RootSignature> reactive_root;
  ComPtr<ID3D12PipelineState> reactive_pipeline;
  uint64_t prior_color_frame = kNone;
  PixelJitter prior_color_jitter{};
  uint64_t compositor_frame = kNone, seal_frame = kNone, color_attempt_frame = kNone;
  uint64_t report_menu_frame = kNone, report_motion_match = kNone;
  uint64_t report_scene_frame = kNone, report_motion_frame = kNone, report_hud_frame = kNone;
  uint64_t report_opaque_frame = kNone;
  uint64_t report_last_gameplay = kNone;
  events::ShaderPair report_scene, report_motion, report_hud, report_opaque;
  std::string_view report_hud_failure;
  uint64_t transition_marker = kNone, transition_frame = kNone, last_scene_present = kNone;
  ComPtr<ID3D12Resource> hud, hud_sample;
  ComPtr<ID3D12Resource> transition_raw;
  ComPtr<ID3D12DescriptorHeap> hud_rtv, hud_heap;
  ComPtr<ID3D12DescriptorHeap> transition_heap;
  ComPtr<ID3D12RootSignature> hud_root;
  ComPtr<ID3D12PipelineState> hud_prepare;
  ComPtr<ID3D12PipelineState> transition_copy;
  uint64_t hud_frame = kNone;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT hud_layout{}, hud_scene_layout{};
  uint64_t hud_to_sample = kNone, hud_sample_frame = kNone, hud_sample_submission = kNone;
  unsigned hud_samples = 0;
  unsigned hud_draws = 0;
  bool hud_sealed = false;
  bool reset_after_presentation_fallback = false;
};

std::unique_ptr<State> state;

struct FallbackTelemetry {
  std::ofstream stream;
  std::string_view provider;
  std::string_view failure_reason;
  std::string failure_detail;
  bool open_range = false;

  void Report(uint64_t frame, std::string_view reason, events::ShaderPair draw = {},
              events::ShaderPair motion = {}) {
    if (!stream) return;
    if (!reason.empty()) {
      events::Write(stream, false, frame, provider, reason, draw, motion, failure_detail);
      open_range = true;
    } else if (open_range) {
      events::Write(stream, true, frame, provider);
      open_range = false;
    }
  }
};

FallbackTelemetry fallback_telemetry;

void Disable(const char* cause) noexcept {
  if (!state) return;
  state->log << "disabled\t" << cause << std::endl;
  state->enabled = false;
  fallback_telemetry.failure_reason = "upscaler-runtime-disabled";
  fallback_telemetry.failure_detail = cause;
}
}  // namespace

void Prepare() noexcept {
  try {
    if (state || fallback_telemetry.stream.is_open()) return;
    const auto directory = Env(L"ASURA_LIVE_DLSS_QA");
    const auto runtime = Env(L"ASURA_LIVE_DLSS_RUNTIME");
    if (directory.empty()) return;
    const auto provider = Env(L"ASURA_LIVE_UPSCALER");
    if (!provider.empty() && provider != L"dlss" && provider != L"fsr3") {
      throw std::runtime_error("Unsupported live upscaler");
    }
    const auto algorithm = provider == L"fsr3" ? Algorithm::Fsr3 : Algorithm::Dlss;
    fallback_telemetry.provider = algorithm == Algorithm::Fsr3 ? "fsr3" : "dlss";
    const auto report_file = events::File(Env(L"ASURA_UPSCALER_REPORT_DIR"),
                                          GetCurrentProcessId(), GetCurrentProcess());
    if (!report_file.empty() && Env(L"ASURA_LIVE_DLSS_PRESENT") == L"1") {
      fallback_telemetry.stream.open(report_file, std::ios::binary | std::ios::app);
      if (fallback_telemetry.stream && fallback_telemetry.stream.tellp() == 0) {
        fallback_telemetry.stream << events::kColumns << std::endl;
      }
    }
    if (runtime.empty()) throw std::runtime_error("The upscaler runtime path is missing");
    state = std::make_unique<State>(directory, runtime, algorithm);
  } catch (const std::exception& failure) {
    const auto directory = Env(L"ASURA_LIVE_DLSS_QA");
    std::ofstream(std::filesystem::path(directory) / "live-temporal-error.txt") << failure.what();
    fallback_telemetry.failure_reason = "upscaler-initialization-failed";
    fallback_telemetry.failure_detail = failure.what();
  }
}

void Shutdown() noexcept {
  state.reset();
  fallback_telemetry = {};
}
bool Active() noexcept { return state && state->enabled; }

bool IsMotionProgram(uint64_t vertex, uint64_t pixel) {
  return std::any_of(std::begin(kPosePrograms), std::end(kPosePrograms), [=](const auto& program) {
    return program.vertex == vertex && program.pixel == pixel;
  });
}

bool IsOpaqueViewProgram(uint64_t vertex) noexcept {
  // Verified against the motion pass in the same 720p scene.
  return vertex == 0x05253eaa754b90a5 || vertex == 0x121a96d38a21bfdb;
}

bool IsCompactMotionProgram(uint64_t vertex, uint64_t pixel) noexcept {
  return vertex == 0x68c798b21d1e8771 && pixel == 0xe7d0056ff4d559ee;
}

bool IsSceneCompositor(uint64_t vertex, uint64_t pixel) {
  return vertex == 0x9ac7fba7d9196e93 && pixel == 0x2e571939e5c4f114;
}

void ObserveCandidate(uint64_t frame, bool compositor, bool match,
                      uint64_t guest_vs, uint64_t guest_ps, std::span<const uint8_t> vertex,
                      std::span<const uint8_t> pixel) {
  if (!Active()) return;
  if (compositor) {
    if (match || state->compositor_frame != frame) {
      state->report_scene_frame = frame;
      state->report_scene = {guest_vs, guest_ps};
    }
  } else {
    if (match || state->report_motion_match != frame) {
      state->report_motion_frame = frame;
      state->report_motion = {guest_vs, guest_ps};
    }
    if (match) state->report_motion_match = frame;
  }
  if (compositor && match) state->compositor_frame = frame;
  const bool episode2_loading_boundary =
      (guest_vs == 0x3bc30231329c87ec && guest_ps == 0xfab54f610eed78f1) ||
      (guest_vs == 0x6b5cbfd17f02a042 && guest_ps == 0xf22521312cd8ef75);
  if (compositor && episode2_loading_boundary) {
    state->transition_marker = frame;
  }
  if (!state->diagnostics || state->candidates.size() >= 40 ||
      vertex.size() < 20 || pixel.size() < 20) return;
  std::string key = compositor ? "compositor" : "pose";
  constexpr std::string_view digits = "0123456789abcdef";
  for (const auto code : {vertex, pixel}) {
    key += '\t';
    for (const auto byte : code.subspan(4, 16)) {
      key += digits[byte >> 4];
      key += digits[byte & 15];
    }
  }
  if (state->candidates.insert(key).second || (match && compositor && frame % 180 == 0)) {
    state->log << "candidate\t" << frame << '\t' << match << '\t' << key
               << "\tguest=" << std::hex << guest_vs << ',' << guest_ps << std::dec
               << "\tdepth=" << (state->depth_frame == frame)
               << "\tcamera=" << (state->camera_frame == frame) << std::endl;
  }
}

bool TraceNearScene(uint64_t frame) noexcept {
  return Active() && state->trace_transitions && state->last_matched != kNone &&
         frame - state->last_matched <= 120;
}

bool SuppressMotionBlur() noexcept {
  static const bool requested = Env(L"ASURA_REMOVE_MOTION_BLUR") == L"1";
  return requested;
}

void TraceBlurSuppressed(uint64_t frame) noexcept {
  if (Active() && state->trace_transitions) state->log << "blur-suppressed\t" << frame << '\n';
}

void TraceResolve(uint64_t frame, bool depth, uint32_t tile, uint32_t address,
                  uint32_t width, uint32_t height) noexcept {
  if (!TraceNearScene(frame) || (width < 1280 && height < 720)) return;
  state->log << "resolve-path\t" << frame << '\t' << (depth ? "depth" : "color")
             << "\ttile=" << tile << "\taddress=" << std::hex << address << std::dec
             << "\tregion=" << width << 'x' << height << '\n';
}

void TraceSwap(uint64_t frame, uint32_t page, uint32_t format,
               uint32_t width, uint32_t height, bool compatible_frontbuffer, bool upscaled) noexcept {
  if (fallback_telemetry.stream.is_open()) {
    const auto draw = [frame](uint64_t observed, events::ShaderPair shaders) {
      return observed == frame ? shaders : events::ShaderPair{};
    };
    if (!fallback_telemetry.failure_reason.empty()) {
      fallback_telemetry.Report(frame, fallback_telemetry.failure_reason,
          state ? draw(state->report_scene_frame, state->report_scene) : events::ShaderPair{},
          state ? draw(state->report_motion_frame, state->report_motion) : events::ShaderPair{});
    } else if (Active()) {
      const bool menu = state->report_menu_frame == frame;
      const bool depth = state->depth_frame == frame;
      const bool camera = state->camera_frame == frame;
      if (upscaled && !menu && depth && camera) state->report_last_gameplay = frame;
      const bool recent = fallback_telemetry.open_range ||
          (state->report_last_gameplay != kNone && frame - state->report_last_gameplay <= 120);
      events::SceneEvidence evidence{
          menu, recent, state->report_motion_match == frame,
          state->opaque_frame == frame && state->opaque_view_count != 0,
          state->compositor_frame == frame, depth, camera, state->color_frame == frame,
          state->seal_frame == frame, compatible_frontbuffer,
          state->report_hud_frame == frame ? state->report_hud_failure : std::string_view{}};
      const auto reason = upscaled ? std::string_view{} : events::FallbackReason(evidence);
      const auto shaders = state->report_hud_frame == frame ? state->report_hud :
          state->report_scene_frame == frame ? state->report_scene :
          draw(state->report_opaque_frame, state->report_opaque);
      fallback_telemetry.Report(frame, reason, shaders,
          draw(state->report_motion_frame, state->report_motion));
    }
  }
  if (!TraceNearScene(frame) || upscaled) return;
  state->log << "native-swap\t" << frame << "\tpage=" << std::hex << page << std::dec
             << "\tformat=" << format << "\tsize=" << width << 'x' << height
             << "\tdepth=" << (state->depth_frame == frame)
             << "\tcompositor=" << (state->compositor_frame == frame)
             << "\tcolor_attempt=" << (state->color_attempt_frame == frame)
             << "\tcolor=" << (state->color_frame == frame)
             << "\tresolved=" << (state->seal_frame == frame)
             << "\tcamera=" << (state->camera_frame == frame)
             << "\topaque_candidates=" << (state->opaque_frame == frame ? state->opaque_view_count : 0)
             << std::endl;
}

PixelJitter Jitter(uint64_t frame) {
  if (!Active() || state->last_matched == kNone || state->last_matched + 1 != frame) return {};
  if (state->reset_after_presentation_fallback) return {};
  return SceneJitter(static_cast<uint32_t>(frame), kRender, kOutput);
}

void ObserveCamera(uint64_t frame, const Matrix& current, const std::optional<Matrix>& previous,
                   const std::optional<ViewOrigin>& origin, const std::optional<ClipXY>& previous_xy,
                   const float* scale, const float* offset) noexcept {
  if (!Active()) return;
  const bool motion = previous.has_value() || previous_xy.has_value();
  if (state->camera_frame == frame &&
      (!motion || state->camera_source == CameraSource::Native ||
       (!previous && state->motion_camera_observed))) return;
  if (!std::all_of(current.begin(), current.end(), [](float value) {
        return std::isfinite(value);
      })) return;
  const float forward_length = current[3] * current[3] + current[7] * current[7] +
                               current[11] * current[11];
  if (!(std::abs(forward_length - 1.0f) < 0.005f) ||
      !(std::abs(XMVectorGetX(XMMatrixDeterminant(Load(current)))) > 0.0001f)) return;

  if (state->camera_view.frame != frame) {
    state->prior_camera_view = state->camera_view;
    state->motion_camera_observed = false;
  }
  state->camera_view = {frame, current, origin};
  state->opaque_view_recovered = false;
  state->motion_camera_observed |= motion;
  state->camera_source = CameraSource::CurrentOnly;
  auto prior = current;
  const auto& saved = state->prior_camera_view;
  if (previous) {
    prior = *previous;
    state->camera_source = CameraSource::Native;
  }

  if (origin && saved.origin && saved.frame != kNone && saved.frame + 1 == frame) {
    const auto positive = RebasePreviousView(saved.projection, *saved.origin, *origin, 1.0f);
    const auto negative = RebasePreviousView(saved.projection, *saved.origin, *origin, -1.0f);
    if (previous) {
      const bool plus = MatchesNativeCamera(positive, *previous);
      const bool minus = MatchesNativeCamera(negative, *previous);
      if (plus != minus) {
        const int old = state->origin_checks;
        state->origin_checks = std::clamp(old + (plus ? 1 : -1), -3, 3);
        if (old != state->origin_checks) {
          state->log << "camera-origin-check\t" << frame << '\t' << state->origin_checks
                     << std::endl;
        }
      }
    } else if (previous_xy && std::abs(state->origin_checks) == 3) {
      const auto& candidate = state->origin_checks > 0 ? positive : negative;
      if (MatchesPreviousXY(candidate, *previous_xy)) {
        prior = candidate;
        state->camera_source = CameraSource::Rebased;
      }
    }
    if (state->diagnostics && state->camera_audit_samples < 96 &&
        ((previous && state->camera_audit_samples < 48 && frame % 7 == 0) ||
         (previous_xy && frame % 3 == 0))) {
      ++state->camera_audit_samples;
      state->log << "camera-rebase-audit\t" << frame << '\t' << (previous ? "full" : "xy");
      LogFloats(state->log, *saved.origin);
      LogFloats(state->log, *origin);
      LogFloats(state->log, saved.projection);
      if (previous) LogFloats(state->log, *previous);
      if (previous_xy) LogFloats(state->log, *previous_xy);
      state->log << std::endl;
    }
  }
  state->current = HostClip(current, scale, offset);
  state->previous = HostClip(prior, scale, offset);
  state->host_basis = HostClip(Store(XMMatrixIdentity()), scale, offset);
  state->camera_frame = frame;
  if (state->diagnostics && previous_xy) {
    state->log << "camera-compact\t" << frame << "\tverified="
               << (state->camera_source == CameraSource::Rebased)
               << "\torigin=" << origin.has_value() << "\tprior_origin=" << saved.origin.has_value()
               << "\tchecks=" << state->origin_checks << std::endl;
  }
}

void ObserveOpaqueCamera(uint64_t frame, uint64_t shader, uint64_t pixel, uint64_t used_registers,
                         std::span<const uint32_t> registers, const float* scale,
                         const float* offset) noexcept {
  if (!Active()) return;
  // These two opaque programs draw the title screen and episode/results menus.
  if (shader == 0x65cb4158eca40e72 || shader == 0x26d8f5aa2555f350) {
    state->report_menu_frame = frame;
  }
  if (state->opaque_frame != frame) {
    state->opaque_frame = frame;
    state->opaque_view_count = 0;
  }
  for (uint32_t first = 0; first + 4 <= registers.size() / 4; ++first) {
    if (((used_registers >> first) & 0xF) != 0xF) continue;
    Matrix view;
    std::memcpy(view.data(), registers.data() + first * 4, sizeof(view));
    if (!IsRebasedSceneProjection(view)) continue;
    if (state->report_opaque_frame != frame) {
      state->report_opaque_frame = frame;
      state->report_opaque = {shader, pixel};
    }
    if (state->camera_frame == frame && MatchesNativeCamera(view, state->camera_view.projection)) {
      state->verified_opaque_layouts.emplace(shader, first);
    }
    auto& count = state->opaque_view_count;
    auto& views = state->opaque_views;
    auto match = std::find_if(views.begin(), views.begin() + count, [&](const auto& candidate) {
      return MatchesNativeCamera(candidate.projection, view);
    });
    if (match == views.begin() + count) {
      if (count == views.size()) return;
      *match = {view, {scale[0], scale[1], scale[2]}, {offset[0], offset[1], offset[2]}, shader, first};
      ++count;
    } else if (shader != match->shader) {
      match->corroborating_shader = shader;
    }
    // The rest of this shader's constants contain material and object state.
    return;
  }
}

void RecoverOpaqueCamera(uint64_t frame) noexcept {
  if (state->camera_frame == frame || state->opaque_frame != frame) return;
  const OpaqueView* best = nullptr;
  int best_score = 0;
  for (size_t i = 0; i < state->opaque_view_count; ++i) {
    const auto& candidate = state->opaque_views[i];
    const bool confirmed = candidate.corroborating_shader ||
        state->verified_opaque_layouts.contains({candidate.shader, candidate.first_register});
    // A shared projection or previously verified layout outranks a single
    // producer using the game's standard c0-c3 camera layout.
    const int score = confirmed ? 2 : candidate.first_register == 0 ? 1 : 0;
    if (score > best_score) {
      best = &candidate;
      best_score = score;
    } else if (score == best_score) {
      best = nullptr;
    }
  }
  if (!best) return;
  ObserveCamera(frame, best->projection, std::nullopt, std::nullopt, std::nullopt,
                best->scale.data(), best->offset.data());
  state->opaque_view_recovered = state->camera_frame == frame;
  if (state->diagnostics && state->opaque_view_recovered) {
    state->log << "camera-opaque\t" << frame << "\tguest=" << std::hex << best->shader << ','
               << best->corroborating_shader << std::dec << "\tc=" << best->first_register
               << "\tverified=" << (best_score == 2) << std::endl;
  }
}

ID3D12Resource* CaptureDepth(ID3D12Device* device, uint64_t frame, ID3D12Resource* source) noexcept {
  if (!Active() || state->depth_frame == frame) return nullptr;
  const auto desc = source->GetDesc();
  if (state->observed_depth_surfaces.insert(
          {static_cast<uint64_t>(desc.Format), desc.Width, desc.Height, desc.SampleDesc.Count}).second) {
    state->log << "depth-candidate\t" << frame << "\tformat=" << desc.Format
               << "\tsize=" << desc.Width << 'x' << desc.Height
               << "\tsamples=" << desc.SampleDesc.Count << std::endl;
  }
  if (desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS || desc.Width != kRender.width ||
      desc.Height < kRender.height || desc.SampleDesc.Count != 1) return nullptr;
  try {
    if (!state->raw_depth) state->Initialize(device, source);
    if (state->raw_depth->GetDesc().Height != desc.Height) return nullptr;
    state->depth_frame = frame;
    return state->raw_depth.Get();
  } catch (const std::exception& failure) {
    Disable(failure.what());
    return nullptr;
  }
}

ID3D12Resource* CaptureColor(ID3D12Device*, uint64_t frame, ID3D12Resource* source) noexcept {
  if (!Active()) return nullptr;
  state->color_attempt_frame = frame;
  if (state->color_frame == frame || state->depth_frame != frame) return nullptr;
  RecoverOpaqueCamera(frame);
  if (state->camera_frame != frame) {
    if (state->last_matched != kNone && frame % 30 == 0) {
      const auto count = state->opaque_frame == frame ? state->opaque_view_count : 0;
      state->log << "camera-missing\t" << frame << "\topaque_views=" << count << std::endl;
    }
    return nullptr;
  }
  const auto desc = source->GetDesc();
  if (state->color_frame == kNone) {
    state->log << "color-candidate\t" << frame << "\tformat=" << desc.Format
               << "\tsize=" << desc.Width << 'x' << desc.Height << std::endl;
  }
  if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.Width != kRender.width ||
      desc.Height < kRender.height || desc.SampleDesc.Count != 1) return nullptr;
  state->color_frame = frame;
  state->pending = true;
  return state->color.Get();
}

ID3D12Resource* CaptureComposedTransition(uint64_t frame, ID3D12Resource* source,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& view) noexcept {
  if (!Active() || !state->present || state->color_frame == frame || state->seal_frame != frame ||
      state->color_attempt_frame != frame || state->depth_frame == frame || state->camera_frame == frame ||
      state->transition_marker == kNone || frame - state->transition_marker > 270 ||
      state->last_scene_present == kNone || frame - state->last_scene_present > 45) {
    return nullptr;
  }
  const auto desc = source->GetDesc();
  if ((desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
      desc.Width != kRender.width || desc.Height != kRender.height || desc.SampleDesc.Count != 1 ||
      view.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      view.Shader4ComponentMapping != kLoadingCardMapping) {
    if (state->trace_transitions) {
      state->log << "transition-format-mismatch\t" << frame << "\tresource=" << desc.Format
                 << "\tview=" << view.Format << "\tswizzle=" << view.Shader4ComponentMapping
                 << "\tsize=" << desc.Width << 'x' << desc.Height << std::endl;
    }
    return nullptr;
  }
  state->transition_frame = frame;
  state->color_frame = frame;
  state->pending = true;
  return state->transition_raw.Get();
}

std::optional<ObjectPoseDraw> BeginObjectPose(ID3D12Device* device, uint64_t frame,
    ID3D12RootSignature* root, uint64_t guest_vs, std::span<const uint8_t> vertex) noexcept {
  if (!Active() || state->depth_frame != frame || state->camera_frame != frame) return std::nullopt;
  try {
    const auto program = std::find_if(std::begin(kPosePrograms), std::end(kPosePrograms),
                                     [=](const auto& value) { return value.vertex == guest_vs; });
    if (program == std::end(kPosePrograms)) return std::nullopt;
    auto* pipeline = state->ObjectPipeline(device, root, vertex, *program);
    if (!pipeline) return std::nullopt;
    const bool clear = state->pose_frame != frame;
    if (clear) state->pose_draws = 0;
    state->pose_frame = frame;
    ++state->pose_draws;
    const auto jitter = Jitter(frame);
    return ObjectPoseDraw{pipeline, state->raw_depth.Get(),
        state->pose_rtv->GetCPUDescriptorHandleForHeapStart(),
        state->pose_dsv->GetCPUDescriptorHandleForHeapStart(),
        {jitter.x - 0.5f, jitter.y - 0.5f, 1280.0f, 720.0f, 0.0f, 0.5f}, clear};
  } catch (const std::exception& failure) {
    state->log << "pose-error\t" << failure.what() << std::endl;
    return std::nullopt;
  }
}

bool HudActive(uint64_t frame) noexcept {
  return Active() && state->hud_frame == frame;
}

bool IsHudProgram(uint64_t vs, uint64_t ps) noexcept {
  // This title uses these programs for captions, prompts, and the HUD in combat.
  return (vs == 0xf9ebe3d4293ec775 && ps == 0x19055c4d86c6c08c) ||
         (vs == 0xe49d26f998b9a5b3 && ps == 0xa7ab2d75c169dfde) ||
         (vs == 0x6eb1d77a55840b2d && ps == 0xb8a5ddaba29740e5);
}

bool IsHudTextureBlend(uint64_t vs, uint64_t ps) noexcept {
  return vs == 0x5bf4e342d00f1593 && ps == 0x2c1e4e099e31e037;
}

void TraceHudTextureBlend(uint64_t frame, std::span<const uint32_t, 8> sources,
                          bool eligible, bool supported) {
  if (!state->diagnostics) return;
  static std::set<std::array<uint32_t, 10>> seen;
  if (seen.size() >= 32) return;
  std::array<uint32_t, 10> key{};
  std::copy(sources.begin(), sources.end(), key.begin());
  key[8] = eligible;
  key[9] = supported;
  if (!seen.insert(key).second) return;
  state->log << "hud-blend-sources\t" << frame << "\teligible=" << eligible
             << "\tsupported=" << supported;
  for (unsigned i : {0u, 4u}) {
    state->log << '\t' << sources[i + 1] << 'x' << sources[i + 2]
               << "\tpage=" << std::hex << sources[i] << std::dec
               << "\tformat=" << sources[i + 3];
  }
  state->log << std::endl;
}

std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> HudTarget(uint64_t frame, bool supported,
                                                     uint64_t vs, uint64_t ps) noexcept {
  if (!HudActive(frame)) return std::nullopt;
  if (!supported || state->hud_sealed) {
    const bool after_resolve = state->hud_sealed;
    state->hud_frame = kNone;
    state->reset_after_presentation_fallback = true;
    const bool scene_blend = IsHudTextureBlend(vs, ps);
    state->report_hud_frame = frame;
    state->report_hud = {vs, ps};
    state->report_hud_failure = after_resolve ? "hud-draw-after-final-resolve" :
        scene_blend ? "hud-two-texture-blend" : "unsupported-hud-draw";
    state->log << "hud-fallback\t" << frame << "\tguest=" << std::hex << vs << ',' << ps
               << std::dec << "\treason=" << (scene_blend ? "two-texture-blend" : "unsupported-draw")
               << std::endl;
    return std::nullopt;
  }
  ++state->hud_draws;
  return state->hud_rtv->GetCPUDescriptorHandleForHeapStart();
}

void SealHud(uint64_t frame) noexcept {
  if (Active()) state->seal_frame = frame;
  if (HudActive(frame)) state->hud_sealed = true;
}

ID3D12Resource* PresentHud(uint64_t frame) noexcept {
  if (!HudActive(frame) || !state->hud_sealed) return nullptr;
  state->hud_frame = kNone;
  return state->hud.Get();
}

void ObserveHudPresent(uint64_t frame) noexcept {
  if (state->transition_frame != frame) state->last_scene_present = frame;
  if (!state->diagnostics && frame % 300 != 0) return;
  state->log << state->provider << "-present\t" << frame << "\t2560x1440\thud_draws=" << state->hud_draws
             << std::endl;
  if (state->diagnostics && state->hud_draws && state->hud_samples < 8 &&
      state->hud_sample_submission == kNone &&
      (state->hud_sample_frame == kNone || frame - state->hud_sample_frame >= 720)) {
    state->hud_to_sample = frame;
  }
}

void Record(ID3D12GraphicsCommandList* list, uint64_t submission, uint64_t completed) noexcept {
  if (!Active()) return;
  try {
    if (state->present) state->RecordHudSample(list, submission, completed);
    if (state->sample_submission != kNone && completed >= state->sample_submission) {
      state->SaveSample();
      state->sample_submission = kNone;
    }
    if (!state->pending) return;
    state->pending = false;
    const auto frame = state->color_frame;
    const bool composed = state->transition_frame == frame;
    const auto jitter = composed ? PixelJitter{} : Jitter(frame);
    const auto& current = composed ? state->last_camera : state->current;
    const float direction = current[3] * state->last_camera[3] +
                            current[7] * state->last_camera[7] +
                            current[11] * state->last_camera[11];
    const bool discontinuity = direction < 0.92f || state->reset_after_presentation_fallback ||
                               state->camera_source == CameraSource::CurrentOnly || composed;
    const auto history = state->history.BeginFrame(static_cast<uint32_t>(frame), current,
        kRender, kOutput, discontinuity, composed ? current : state->previous);
    const auto camera = DescribeCamera(current, history);
    Transition(list, state->raw_depth.Get(), kCopy, kRead);
    if (composed) {
      state->PrepareTransitionColor(list);
    } else {
      Transition(list, state->color.Get(), kCopy, kRead);
    }
    const bool use_pose = !composed && state->pose_frame == frame;
    Transition(list, state->pose.Get(), kPoseTarget, kRead);
    const auto pose_to_previous = Store(Load(state->host_basis) * Load(history.clip_to_previous_clip));
    state->motion_pass->Record(list,
        {history.clip_to_previous_clip, kRender.width, kRender.height, jitter, {0, 0.5f},
         history.reset, use_pose ? 2u : 0u, true, composed, {}, pose_to_previous}, state->heap.Get(),
         state->heap->GetGPUDescriptorHandleForHeapStart(), state->uav_table);
    Transition(list, state->pose.Get(), kRead, kPoseTarget);
    Transition(list, state->motion.Get(), kWrite, kRead);
    Transition(list, state->depth.Get(), kWrite, kRead);
    auto* reactive = state->PrepareReactiveMask(list, frame, jitter, history.reset);
    state->upscaler->Dispatch(list, {state->color.Get(), state->depth.Get(), state->motion.Get(),
                              state->output.Get(), reactive, kRender, camera, uint32_t(frame),
                              jitter.x, jitter.y, 1000.0f / 30.0f, history.reset});
    if (reactive) Transition(list, reactive, kRead, kWrite);
    if (state->present) {
      state->PrepareHud(list, frame);
      // A composition copied from the guest frontbuffer already contains its overlays.
      if (composed) state->hud_sealed = true;
    }
    Transition(list, state->depth.Get(), kRead, kWrite);
    Transition(list, state->motion.Get(), kRead, kWrite);
    state->ReleaseSceneColor(list, frame, jitter, composed);
    Transition(list, state->raw_depth.Get(), kRead, kCopy);
    if (state->diagnostics && state->samples < 24 && state->sample_submission == kNone &&
        (state->reactive_enabled
          ? reactive && (state->samples < 2 || frame > state->sample_frame + 480)
          : history.reset || state->samples < 2 || frame > state->sample_frame + 180)) {
      const auto copy = [&](ID3D12Resource* texture, D3D12_RESOURCE_STATES before,
                            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
        Transition(list, texture, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION dest{state->sample.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dest.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);
        Transition(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, before);
      };
      copy(state->output.Get(), kWrite, state->layout);
      if (use_pose) copy(state->pose.Get(), kPoseTarget, state->pose_layout);
      copy(state->motion.Get(), kWrite, state->motion_layout);
      copy(state->depth.Get(), kWrite, state->depth_layout);
      if (reactive) copy(reactive, kWrite, state->reactive_layout);
      state->sample_history = history;
      state->sample_jitter = jitter;
      state->sample_has_pose = use_pose;
      state->sample_has_reactive = reactive != nullptr;
      state->sample_frame = frame;
      state->sample_submission = submission;
      ++state->samples;
    } else {
      D3D12_RESOURCE_BARRIER fence{};
      fence.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      fence.UAV.pResource = state->output.Get();
      list->ResourceBarrier(1, &fence);
    }
    state->last_matched = frame;
    state->reset_after_presentation_fallback = composed;
    if (!composed) state->last_camera = current;
    if (!state->diagnostics && frame % 300 != 0) return;
    state->log << state->provider << "-prehud\t" << frame << "\treset=" << history.reset
               << "\tjitter=" << jitter.x << ',' << jitter.y
               << "\tcamera_dot=" << direction << "\tobject_draws="
               << (use_pose ? state->pose_draws : 0)
               << "\tcamera_source=" << (composed ? "composed" :
                   state->camera_source == CameraSource::Native ? "native" :
                   state->camera_source == CameraSource::Rebased ? "rebased" :
                   state->opaque_view_recovered ? "opaque" : "reset")
               << "\treactive_mask=" << (reactive != nullptr) << std::endl;
  } catch (const std::exception& failure) {
    Disable(failure.what());
  }
}

}  // namespace asura::temporal::live
