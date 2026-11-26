#include "temporal_d3d12.h"
#include "temporal_camera.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using asura::temporal::Algorithm;
using asura::temporal::Camera;
using asura::temporal::CameraFrame;
using asura::temporal::CameraHistory;
using asura::temporal::DepthMotionPass;
using asura::temporal::DlssPreset;
using asura::temporal::Extent;
using asura::temporal::Frame;
using asura::temporal::Quality;
using asura::temporal::Settings;
using asura::temporal::Upscaler;
using Microsoft::WRL::ComPtr;

namespace {

void Require(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    throw std::runtime_error(std::string(operation) + " returned " + std::to_string(result));
  }
}

ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* device, Extent size, DXGI_FORMAT format,
                                  D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = size.width;
  desc.Height = size.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> texture;
  Require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&texture)), "Creating test texture");
  return texture;
}

void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  commands->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> UploadSceneInput(ID3D12Device* device, ID3D12GraphicsCommandList* commands,
                                       ID3D12Resource* texture, const std::filesystem::path& path) {
  const auto description = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
  UINT rows = 0;
  UINT64 row_bytes = 0;
  UINT64 size = 0;
  device->GetCopyableFootprints(&description, 0, 1, 0, &layout, &rows, &row_bytes, &size);
  if (std::filesystem::file_size(path) != row_bytes * rows)
    throw std::runtime_error("Captured input has the wrong size");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Could not open captured input");

  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC upload_description{};
  upload_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  upload_description.Width = size;
  upload_description.Height = 1;
  upload_description.DepthOrArraySize = 1;
  upload_description.MipLevels = 1;
  upload_description.SampleDesc.Count = 1;
  upload_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> upload;
  Require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &upload_description,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&upload)), "Creating capture upload");
  void* mapping = nullptr;
  const D3D12_RANGE unread{0, 0};
  Require(upload->Map(0, &unread, &mapping), "Mapping capture upload");
  auto* row = static_cast<char*>(mapping) + layout.Offset;
  for (UINT index = 0; index < rows; ++index) {
    input.read(row, static_cast<std::streamsize>(row_bytes));
    row += layout.Footprint.RowPitch;
  }
  upload->Unmap(0, nullptr);
  if (!input) throw std::runtime_error("Captured input ended early");
  const D3D12_TEXTURE_COPY_LOCATION destination{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION source{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  source.PlacedFootprint = layout;
  commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  return upload;
}

struct TextureReadback {
  ComPtr<ID3D12Resource> buffer;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};

  void Copy(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* texture) {
    UINT64 readback_size = 0;
    const auto texture_desc = texture->GetDesc();
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &layout, nullptr, nullptr, &readback_size);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = readback_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&buffer)), "Creating readback buffer");
    D3D12_TEXTURE_COPY_LOCATION destination{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = layout;
    const D3D12_TEXTURE_COPY_LOCATION source{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  }

  template <size_t N>
  std::array<float, N> Sample(uint32_t x, uint32_t y) {
    void* mapped = nullptr;
    Require(buffer->Map(0, nullptr, &mapped), "Mapping GPU result");
    const auto offset = layout.Offset + y * layout.Footprint.RowPitch + x * sizeof(uint16_t) * N;
    uint16_t half[N]{};
    std::memcpy(half, static_cast<const char*>(mapped) + offset, sizeof(half));
    const D3D12_RANGE unwritten{0, 0};
    buffer->Unmap(0, &unwritten);
    std::array<float, N> values{};
    for (size_t i = 0; i < N; ++i) {
      values[i] = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
    }
    return values;
  }

  float SampleFloat(uint32_t x, uint32_t y) {
    void* mapped = nullptr;
    Require(buffer->Map(0, nullptr, &mapped), "Mapping GPU depth");
    const auto offset = layout.Offset + y * layout.Footprint.RowPitch + x * sizeof(float);
    float depth;
    std::memcpy(&depth, static_cast<const char*>(mapped) + offset, sizeof(depth));
    const D3D12_RANGE unwritten{0, 0};
    buffer->Unmap(0, &unwritten);
    return depth;
  }
};

Camera MakeCamera(Extent render) {
  using namespace DirectX;
  Camera camera{};
  camera.near_plane = 0.1f;
  camera.far_plane = 100.0f;
  camera.vertical_fov = XM_PIDIV4;
  camera.up = {0, 1, 0};
  camera.right = {1, 0, 0};
  camera.forward = {0, 0, 1};
  camera.view_space_to_meters = 1.0f;
  const auto projection = XMMatrixPerspectiveFovLH(
      camera.vertical_fov, float(render.width) / float(render.height),
      camera.near_plane, camera.far_plane);
  XMFLOAT4X4 matrix{};
  XMStoreFloat4x4(&matrix, projection);
  std::memcpy(camera.view_to_clip.data(), &matrix, sizeof(matrix));
  XMStoreFloat4x4(&matrix, XMMatrixInverse(nullptr, projection));
  std::memcpy(camera.clip_to_view.data(), &matrix, sizeof(matrix));
  XMStoreFloat4x4(&matrix, XMMatrixIdentity());
  std::memcpy(camera.clip_to_previous_clip.data(), &matrix, sizeof(matrix));
  camera.previous_clip_to_clip = camera.clip_to_previous_clip;
  return camera;
}

void Probe(Settings settings, const std::filesystem::path& runtime,
           const std::optional<std::filesystem::path>& scene) {
  asura::temporal::DepthRange depth_range{0.0f, 0.5f};
  if (scene) {
    settings.inverted_depth = true;
    depth_range = {0.0f, 1.0f};
    const auto range_path = *scene / "scene-depth-range.txt";
    if (std::filesystem::exists(range_path)) {
      std::ifstream range_input(range_path);
      if (!(range_input >> depth_range.min >> depth_range.max) ||
          !(depth_range.min >= 0.0f && depth_range.min < depth_range.max &&
            depth_range.max <= 1.0f)) {
        throw std::runtime_error("Invalid depth-producing viewport in captured scene");
      }
    }
  }
  constexpr auto input_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  constexpr auto target = D3D12_RESOURCE_STATE_RENDER_TARGET;
  constexpr auto compute_read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto unordered_access = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ComPtr<ID3D12Device> device;
  Upscaler upscaler(settings, runtime);
  Require(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
          "Creating D3D12 device");
  const auto render = upscaler.Initialize(device.Get());
  if (scene && (render.width != 1280 || render.height != 720))
    throw std::runtime_error("The captured scene requires native 1280x720 input");
  std::printf("Render %ux%u, output %ux%u\n", render.width, render.height,
              settings.output.width, settings.output.height);

  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  Require(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "Creating queue");
  Require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "Creating allocator");
  Require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)), "Creating command list");

  constexpr auto color_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  auto color = MakeTexture(device.Get(), render, color_format, input_flags, target);
  auto depth = MakeTexture(device.Get(), render, DXGI_FORMAT_R32_FLOAT, input_flags, target);
  constexpr auto motion_format = DXGI_FORMAT_R16G16_FLOAT;
  auto object_motion = MakeTexture(device.Get(), render, motion_format, input_flags, target);
  auto object_valid = MakeTexture(device.Get(), render, DXGI_FORMAT_R32_UINT, input_flags, target);
  auto motion = MakeTexture(device.Get(), render, motion_format,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, unordered_access);
  auto normalized_depth = MakeTexture(device.Get(), render, DXGI_FORMAT_R32_FLOAT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, unordered_access);
  auto output = MakeTexture(device.Get(), settings.output, color_format,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, unordered_access);
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  Require(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)), "Creating RTV heap");
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  const UINT rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  ID3D12Resource* inputs[] = {color.Get(), depth.Get(), object_motion.Get(), object_valid.Get()};
  std::array<ComPtr<ID3D12Resource>, 2> scene_uploads;
  const float values[][4] = {{0.25f, 0.5f, 0.75f, 1.0f}, {0.45f, 0, 0, 0},
                             {3.0f, -5.0f, 0, 0}, {0, 0, 0, 0}};
  for (size_t i = 0; i < std::size(inputs); ++i) {
    if (scene && i < scene_uploads.size()) {
      Transition(commands.Get(), inputs[i], target, D3D12_RESOURCE_STATE_COPY_DEST);
      scene_uploads[i] = UploadSceneInput(device.Get(), commands.Get(), inputs[i],
                                          *scene / (i == 0 ? "scene-rgba16f.bin" : "depth-r32f.bin"));
      Transition(commands.Get(), inputs[i], D3D12_RESOURCE_STATE_COPY_DEST, compute_read);
      rtv.ptr += rtv_stride;
      continue;
    }
    device->CreateRenderTargetView(inputs[i], nullptr, rtv);
    commands->ClearRenderTargetView(rtv, values[i], 0, nullptr);
    if (inputs[i] == object_valid.Get()) {
      const D3D12_RECT object_pixel{0, 0, 1, 1};
      const float valid[4] = {1, 0, 0, 0};
      commands->ClearRenderTargetView(rtv, valid, 1, &object_pixel);
    }
    Transition(commands.Get(), inputs[i], target, compute_read);
    rtv.ptr += rtv_stride;
  }

  const D3D12_DESCRIPTOR_HEAP_DESC motion_heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  ComPtr<ID3D12DescriptorHeap> motion_heap;
  Require(device->CreateDescriptorHeap(&motion_heap_desc, IID_PPV_ARGS(&motion_heap)),
          "Creating motion descriptors");
  auto motion_descriptor = motion_heap->GetCPUDescriptorHandleForHeapStart();
  const UINT view_stride =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (const size_t i : {1, 2, 3}) {
    device->CreateShaderResourceView(inputs[i], nullptr, motion_descriptor);
    motion_descriptor.ptr += view_stride;
  }
  device->CreateUnorderedAccessView(motion.Get(), nullptr, nullptr, motion_descriptor);
  motion_descriptor.ptr += view_stride;
  device->CreateUnorderedAccessView(normalized_depth.Get(), nullptr, nullptr, motion_descriptor);
  const auto motion_srv = motion_heap->GetGPUDescriptorHandleForHeapStart();
  const D3D12_GPU_DESCRIPTOR_HANDLE motion_uav{motion_srv.ptr + 3 * view_stride};

  DepthMotionPass motion_pass(device.Get());
  CameraHistory history;
  Frame frame{color.Get(), normalized_depth.Get(), motion.Get(), output.Get(), nullptr, render,
              MakeCamera(render), 0, 0, 0, 1000.0f / 30.0f, true};
  const auto set_camera = [&](const CameraFrame& camera) {
    frame.camera.clip_to_previous_clip = camera.clip_to_previous_clip;
    frame.camera.previous_clip_to_clip = camera.previous_clip_to_clip;
    frame.jitter_x = camera.jitter.x;
    frame.jitter_y = camera.jitter.y;
    frame.reset_history = camera.reset;
    motion_pass.Record(commands.Get(),
                       {camera.clip_to_previous_clip, render.width, render.height, camera.jitter,
                        depth_range, camera.reset, !scene, settings.inverted_depth},
                       motion_heap.Get(), motion_srv, motion_uav);
    Transition(commands.Get(), motion.Get(), unordered_access, compute_read);
    Transition(commands.Get(), normalized_depth.Get(), unordered_access, compute_read);
  };
  const auto projection = frame.camera.view_to_clip;
  const auto initial = history.BeginFrame(frame.index, projection, render, settings.output, false);
  if (!initial.reset || (initial.jitter.x == 0 && initial.jitter.y == 0)) {
    throw std::runtime_error("First scene frame failed to reset history or select jitter");
  }
  auto first_camera = initial;
  if (scene) first_camera.jitter = {};
  set_camera(first_camera);
  upscaler.Dispatch(commands.Get(), frame);
  if (!scene) {
    D3D12_RESOURCE_BARRIER memory_barrier{};
    memory_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    memory_barrier.UAV.pResource = output.Get();
    commands->ResourceBarrier(1, &memory_barrier);
    Transition(commands.Get(), motion.Get(), compute_read, unordered_access);
    Transition(commands.Get(), normalized_depth.Get(), compute_read, unordered_access);
    ++frame.index;
    const auto shifted = asura::temporal::ApplyProjectionJitter(projection, {4.0f, -2.0f}, render);
    frame.camera.view_to_clip = shifted;
    DirectX::XMFLOAT4X4 view_projection;
    std::memcpy(&view_projection, shifted.data(), sizeof(view_projection));
    DirectX::XMStoreFloat4x4(&view_projection,
        DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&view_projection)));
    std::memcpy(frame.camera.clip_to_view.data(), &view_projection, sizeof(view_projection));
    const auto next = history.BeginFrame(frame.index, shifted, render, settings.output, false);
    const auto duplicate = history.BeginFrame(frame.index, shifted, render, settings.output, false);
    const auto cut = history.BeginFrame(frame.index + 1, projection, render, settings.output, true);
    if (next.reset || duplicate.jitter.x != next.jitter.x || duplicate.jitter.y != next.jitter.y ||
        !cut.reset || cut.jitter.x != initial.jitter.x || cut.jitter.y != initial.jitter.y) {
      throw std::runtime_error("Scene frame tracking did not preserve or reset jitter as expected");
    }
    CameraHistory rebased_history;
    rebased_history.BeginFrame(0, projection, render, settings.output, false);
    // In a rebased scene, the current and cached projections can be identical.
    // Supply the previous camera relative to the current origin to retain motion.
    const auto earlier =
        asura::temporal::ApplyProjectionJitter(projection, {-4.0f, 2.0f}, render);
    const auto rebased = rebased_history.BeginFrame(1, projection, render, settings.output, false,
                                                   earlier);
    const auto rebased_repeat = rebased_history.BeginFrame(1, projection, render, settings.output,
                                                          false, earlier);
    const auto rebased_cut = rebased_history.BeginFrame(2, projection, render, settings.output,
                                                       true, earlier);
    for (size_t i = 0; i < rebased.clip_to_previous_clip.size(); ++i) {
      if (std::abs(rebased.clip_to_previous_clip[i] - next.clip_to_previous_clip[i]) > 0.0001f ||
          std::abs(rebased.previous_clip_to_clip[i] - next.previous_clip_to_clip[i]) > 0.0001f) {
        throw std::runtime_error("The rebased camera changed the pixel-motion transform");
      }
    }
    if (rebased.reset || rebased_repeat.reset || rebased.jitter.x != rebased_repeat.jitter.x ||
        rebased.jitter.y != rebased_repeat.jitter.y || !rebased_cut.reset) {
      throw std::runtime_error("The rebased camera failed frame tracking");
    }
    // Exercise X/Y and the unobserved Z/W columns with a shift on all three axes.
    auto expected = projection;
    for (size_t col = 0; col < 4; ++col) {
      expected[12 + col] += 3 * projection[col] - 7 * projection[4 + col] +
                            11 * projection[8 + col];
    }
    const asura::temporal::ClipXY xy{expected[0], expected[1], expected[4], expected[5],
                                    expected[8], expected[9], expected[12], expected[13]};
    const asura::temporal::ViewOrigin from{100000, -200000, 300000};
    const asura::temporal::ViewOrigin to{100003, -200007, 300011};
    const auto recovered = asura::temporal::RebasePreviousView(projection, from, to, 1.0f);
    if (!asura::temporal::MatchesPreviousXY(recovered, xy) ||
        asura::temporal::MatchesPreviousXY(
            asura::temporal::RebasePreviousView(projection, from, to, -1.0f), xy) ||
        std::abs(recovered[14] - expected[14]) > 0.0001f ||
        std::abs(recovered[15] - expected[15]) > 0.0001f) {
      throw std::runtime_error("World-origin recovery did not reproduce the full previous matrix");
    }
    frame.camera = MakeCamera(render);
    set_camera(rebased);
    upscaler.Dispatch(commands.Get(), frame);
  }
  Transition(commands.Get(), output.Get(), unordered_access, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(commands.Get(), motion.Get(), compute_read, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(commands.Get(), normalized_depth.Get(), compute_read, D3D12_RESOURCE_STATE_COPY_SOURCE);

  TextureReadback readback;
  TextureReadback motion_readback;
  TextureReadback depth_readback;
  TextureReadback behind_readback;
  TextureReadback bounded_readback;
  readback.Copy(device.Get(), commands.Get(), output.Get());
  motion_readback.Copy(device.Get(), commands.Get(), motion.Get());
  depth_readback.Copy(device.Get(), commands.Get(), normalized_depth.Get());
  if (!scene) {
    const auto sample_projection = [&](CameraFrame camera, TextureReadback& capture) {
      constexpr auto copy_source = D3D12_RESOURCE_STATE_COPY_SOURCE;
      Transition(commands.Get(), motion.Get(), copy_source, unordered_access);
      Transition(commands.Get(), normalized_depth.Get(), copy_source, unordered_access);
      set_camera(camera);
      Transition(commands.Get(), motion.Get(), compute_read, copy_source);
      Transition(commands.Get(), normalized_depth.Get(), compute_read, copy_source);
      capture.Copy(device.Get(), commands.Get(), motion.Get());
    };
    CameraFrame behind{};
    behind.clip_to_previous_clip[15] = -1.0f;
    sample_projection(behind, behind_readback);
    CameraFrame near_lens{};
    near_lens.clip_to_previous_clip[12] = 10.0f;
    near_lens.clip_to_previous_clip[15] = 0.00001f;
    sample_projection(near_lens, bounded_readback);
  }

  ComPtr<ID3D12Fence> fence;
  Require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Creating fence");
  Require(commands->Close(), "Closing command list");
  ID3D12CommandList* lists[] = {commands.Get()};
  queue->ExecuteCommandLists(1, lists);
  Require(queue->Signal(fence.Get(), 1), "Signaling dispatch completion");
  const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) throw std::runtime_error("Could not create fence event");
  const HRESULT event_status = fence->SetEventOnCompletion(1, event);
  const auto wait_status = SUCCEEDED(event_status) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
  CloseHandle(event);
  if (wait_status != WAIT_OBJECT_0) throw std::runtime_error("Temporal dispatch did not complete");
  Require(device->GetDeviceRemovedReason(), "Checking D3D12 device");

  if (scene) {
    const uint32_t center_x = render.width / 2;
    const uint32_t center_y = render.height / 2;
    std::ifstream input(*scene / "depth-r32f.bin", std::ios::binary);
    input.seekg((center_y * render.width + center_x) * sizeof(float));
    float expected_depth = 0;
    input.read(reinterpret_cast<char*>(&expected_depth), sizeof(expected_depth));
    expected_depth = (expected_depth - depth_range.min) / (depth_range.max - depth_range.min);
    const auto depth_value = depth_readback.SampleFloat(center_x, center_y);
    const auto velocity = motion_readback.Sample<2>(center_x, center_y);
    if (!input || !(std::abs(depth_value - expected_depth) < 0.000001f) ||
        velocity[0] != 0 || velocity[1] != 0)
      throw std::runtime_error("The captured depth or reset-frame motion did not reach the SDK");

    float darkest = std::numeric_limits<float>::infinity();
    float brightest = -std::numeric_limits<float>::infinity();
    for (uint32_t y = 1; y < 8; ++y) {
      for (uint32_t x = 1; x < 8; ++x) {
        const auto color = readback.Sample<4>(x * settings.output.width / 8,
                                             y * settings.output.height / 8);
        if (!std::isfinite(color[0]) || !std::isfinite(color[1]) || !std::isfinite(color[2]))
          throw std::runtime_error("The captured scene produced an invalid output pixel");
        const float luminance = color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
        darkest = std::min(darkest, luminance);
        brightest = std::max(brightest, luminance);
      }
    }
    if (brightest - darkest < 0.01f)
      throw std::runtime_error("The captured scene produced flat or blank output");
    std::printf("PASS captured scene reset dispatch; depth %.7f, output luminance %.5f..%.5f\n",
                depth_value, darkest, brightest);
    return;
  }

  const auto [camera_x, camera_y] = motion_readback.Sample<2>(render.width / 2, render.height / 2);
  const auto [object_x, object_y] = motion_readback.Sample<2>(0, 0);
  if (!(std::abs(camera_x + 4.0f) < 0.02f && std::abs(camera_y - 2.0f) < 0.02f &&
        object_x == 3.0f && object_y == -5.0f)) {
    std::fprintf(stderr, "Motion readback camera=(%.4f, %.4f) object=(%.4f, %.4f)\n",
                 camera_x, camera_y, object_x, object_y);
    throw std::runtime_error("GPU motion vectors did not match the jitter-free test vectors");
  }
  std::printf("PASS GPU motion camera=(%.2f, %.2f) object=(%.2f, %.2f)\n",
              camera_x, camera_y, object_x, object_y);
  const auto behind = behind_readback.Sample<2>(render.width / 2, render.height / 2);
  const auto bounded = bounded_readback.Sample<2>(render.width / 2, render.height / 2);
  if (behind[0] != -float(render.width) || behind[1] != -float(render.height) ||
      bounded[0] != float(render.width) || !std::isfinite(bounded[1])) {
    throw std::runtime_error("Near-plane motion did not reject unavailable history safely");
  }
  std::printf("PASS near-plane history rejection and bounded GPU motion\n");
  const auto normalized = depth_readback.SampleFloat(render.width / 2, render.height / 2);
  if (!(std::abs(normalized - 0.9f) < 0.0001f)) {
    throw std::runtime_error("GPU depth did not normalize from the viewport depth range");
  }
  std::printf("PASS GPU normalized depth %.4f\n", normalized);
  const auto [red, green, blue, alpha] =
      readback.Sample<4>(settings.output.width / 2, settings.output.height / 2);
  if (!(red > 0.1f && red < green && green < blue && blue < 1.1f)) {
    throw std::runtime_error("GPU output failed the synthetic color check");
  }
  std::printf("PASS GPU readback RGB %.4f %.4f %.4f\n", red, green, blue);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc < 4 || argc > 7) {
      throw std::invalid_argument("Use temporal_probe <dlaa|dlss|fsr3|fsr4> "
                                  "<default|J|K|L|M> <vendor-runtime-directory> "
                                  "[quality|balanced|performance|ultra-performance|fixed-720p] "
                                  "[720p|1080p|1440p|2160p] [captured-scene-directory]");
    }
    const std::wstring_view mode = argv[1];
    const std::wstring_view preset = argv[2];
    Settings settings{Algorithm::Dlss, Quality::Quality, DlssPreset::Default,
                      {1280, 720}, true, false};
    if (mode == L"dlaa") settings.algorithm = Algorithm::Dlaa;
    else if (mode == L"fsr3") settings.algorithm = Algorithm::Fsr3;
    else if (mode == L"fsr4") settings.algorithm = Algorithm::Fsr4;
    else if (mode != L"dlss") throw std::invalid_argument("Unknown upscaler");
    if (preset == L"J") settings.preset = DlssPreset::J;
    else if (preset == L"K") settings.preset = DlssPreset::K;
    else if (preset == L"L") settings.preset = DlssPreset::L;
    else if (preset == L"M") settings.preset = DlssPreset::M;
    else if (preset != L"default") throw std::invalid_argument("Unknown DLSS preset");
    if (argc >= 5) {
      const std::wstring_view quality = argv[4];
      if (quality == L"fixed-720p") {
        settings.fixed_render = Extent{1280, 720};
        settings.output = {2560, 1440};
      } else if (quality == L"balanced") settings.quality = Quality::Balanced;
      else if (quality == L"performance") settings.quality = Quality::Performance;
      else if (quality == L"ultra-performance") settings.quality = Quality::UltraPerformance;
      else if (quality != L"quality") throw std::invalid_argument("Unknown quality mode");
    }
    if (argc >= 6) {
      const std::wstring_view output = argv[5];
      if (output == L"720p") settings.output = {1280, 720};
      else if (output == L"1080p") settings.output = {1920, 1080};
      else if (output == L"1440p") settings.output = {2560, 1440};
      else if (output == L"2160p") settings.output = {3840, 2160};
      else throw std::invalid_argument("Unknown output resolution");
    }
    const auto scene = argc == 7 ? std::optional<std::filesystem::path>(argv[6]) : std::nullopt;
    Probe(settings, argv[3], scene);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
