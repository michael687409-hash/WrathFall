#include "gpu/xenos/host/vulkan/vulkan_objects.h"
#include "gpu/xenos/vulkan/command_processor.h"
#include "live_temporal.h"
#include "temporalCompose.h"
#include "temporalHistory.h"
#include "temporalMotion.h"
#include "temporalUnpack.h"

#include <DirectXMath.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace wrathfall::vulkan_live {
namespace {

using namespace rex::gpu::vulkan;
using namespace DirectX;
using asura::temporal::Camera;
using Matrix = std::array<float, 16>;
using Clock = std::chrono::steady_clock;

constexpr VkExtent2D kRender{1280, 720}, kOutput{2560, 1440};
constexpr VkDeviceSize kInputsSize = 2 * VkDeviceSize(1280) * 720 * sizeof(uint32_t);
constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr auto kGeneral = VK_IMAGE_LAYOUT_GENERAL;
constexpr auto kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr auto kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr auto kSampled = VK_ACCESS_SHADER_READ_BIT;
constexpr auto kWritten = VK_ACCESS_SHADER_WRITE_BIT;
constexpr VkDeviceSize kCounterBytes = 4 * sizeof(uint32_t);
constexpr VkDeviceSize kParametersBytes = kCounterBytes + 256 * sizeof(uint32_t);
constexpr Matrix kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

struct PushConstants {
  Matrix warp = kIdentity;
  std::array<float, 2> jitter{}, old_jitter{};
  std::array<int32_t, 2> allocation{};
  uint32_t flags = 0;
};
static_assert(sizeof(PushConstants) == 92);

struct OwnedImage {
  VulkanUpscaleImage resource;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct CounterBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  const uint32_t* mapped = nullptr;
  uint64_t frame = 0;
  bool coherent = false;
};

void Check(VkResult result, const char* step) {
  if (result != VK_SUCCESS) throw std::runtime_error(std::string(step) + " (" + std::to_string(result) + ")");
}

XMMATRIX MatrixLoad(const Matrix& data) {
  return XMMATRIX(data.data());
}

Matrix MatrixStore(FXMMATRIX value) {
  XMFLOAT4X4 matrix;
  XMStoreFloat4x4(&matrix, value);
  Matrix data;
  std::memcpy(data.data(), &matrix, sizeof(matrix));
  return data;
}

Camera MakeCamera(const Matrix& current, const Matrix& prior, bool valid, bool reset) {
  Camera camera{};
  camera.near_plane = 0.1f;
  camera.far_plane = 100.0f;
  camera.vertical_fov = 1.0f;
  camera.view_space_to_meters = 1.0f;
  camera.right = {1, 0, 0};
  camera.up = {0, 1, 0};
  camera.forward = {0, 0, 1};
  camera.view_to_clip = MatrixStore(XMMatrixPerspectiveFovLH(camera.vertical_fov, 16.0f / 9.0f,
                                                            camera.far_plane, camera.near_plane));
  camera.clip_to_previous_clip = camera.previous_clip_to_clip = kIdentity;
  if (valid) {
    const auto fwd = XMVector3Normalize(XMVectorSet(current[3], current[7], current[11], 0));
    const auto axis = [&](size_t i) {
      return XMVectorSet(current[i], current[4 + i], current[8 + i], 0);
    };
    const auto vector = [](FXMVECTOR v) {
      return std::array<float, 3>{XMVectorGetX(v), XMVectorGetY(v), XMVectorGetZ(v)};
    };
    const auto right = axis(0) - fwd * XMVectorGetX(XMVector3Dot(axis(0), fwd));
    const auto up = axis(1) - fwd * XMVectorGetX(XMVector3Dot(axis(1), fwd));
    const float scale_x = XMVectorGetX(XMVector3Length(right));
    const float scale_y = XMVectorGetX(XMVector3Length(up));
    camera.forward = vector(fwd);
    camera.right = vector(XMVector3Normalize(right));
    camera.up = vector(XMVector3Normalize(up));
    camera.vertical_fov = 2 * std::atan(1 / scale_y);
    const float z = -camera.near_plane / (camera.far_plane - camera.near_plane);
    camera.view_to_clip = {scale_x, 0, 0, 0, 0, scale_y, 0, 0, 0, 0, z, 1,
                           0, 0, -z * camera.far_plane, 0};
    if (!reset) {
      const XMMATRIX reverse_depth{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1};
      const auto warp = reverse_depth * XMMatrixInverse(nullptr, MatrixLoad(current)) *
          MatrixLoad(prior) * reverse_depth;
      const auto inverse = XMMatrixInverse(nullptr, warp);
      if (!XMMatrixIsNaN(warp) && !XMMatrixIsInfinite(warp) &&
          !XMMatrixIsNaN(inverse) && !XMMatrixIsInfinite(inverse)) {
        camera.clip_to_previous_clip = MatrixStore(warp);
        camera.previous_clip_to_clip = MatrixStore(inverse);
      }
    }
  }
  camera.clip_to_view = MatrixStore(XMMatrixInverse(nullptr, MatrixLoad(camera.view_to_clip)));
  return camera;
}

}  // namespace

struct Temporal::State {
  static constexpr size_t kSlots = 8;
  enum Image { Color, Depth, Motion, History, Output, Reactive, Count };
  enum Pass { Unpack, EstimateMotion, Compose, StoreHistory };

  explicit State(const VulkanCommandProcessor& processor_in)
      : processor(processor_in), vulkan(*processor.GetVulkanDevice()),
        vk(vulkan.GetFunctions()), device(vulkan.GetDevice()) {}

  ~State() {
    for (const auto pipeline : pipelines) vk.vkDestroyPipeline(device, pipeline, nullptr);
    vk.vkDestroyDescriptorPool(device, pool, nullptr);
    vk.vkDestroyPipelineLayout(device, layout, nullptr);
    vk.vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
    vk.vkDestroySampler(device, sampler, nullptr);
    for (const auto& image : images) {
      vk.vkDestroyImageView(device, image.resource.view, nullptr);
      vk.vkDestroyImage(device, image.resource.image, nullptr);
      vk.vkFreeMemory(device, image.memory, nullptr);
    }
    for (const auto& counter : counters) {
      if (counter.mapped) vk.vkUnmapMemory(device, counter.memory);
      vk.vkDestroyBuffer(device, counter.buffer, nullptr);
      vk.vkFreeMemory(device, counter.memory, nullptr);
    }
    vk.vkDestroyBuffer(device, inputs, nullptr);
    vk.vkFreeMemory(device, input_memory, nullptr);
  }

  void Init() {
    update_buffer = reinterpret_cast<PFN_vkCmdUpdateBuffer>(
        vulkan.GetVulkanInstance()->GetFunctions().vkGetDeviceProcAddr(device, "vkCmdUpdateBuffer"));
    if (!update_buffer) throw std::runtime_error("Vulkan buffer update command is unavailable");
    if (!objects::CreateDedicatedAllocationBuffer(&vulkan, kInputsSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            objects::MemoryPurpose::kDeviceLocal, inputs, input_memory)) {
      throw std::runtime_error("Allocate Vulkan scene and depth snapshots");
    }
    VkSamplerCreateInfo sample{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sample.magFilter = sample.minFilter = VK_FILTER_LINEAR;
    sample.addressModeU = sample.addressModeV = sample.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vk.vkCreateSampler(device, &sample, nullptr, &sampler), "Create Vulkan presentation sampler");

    constexpr auto storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr auto sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storage, storage, storage,
                                      storage, sampled, sampled, storage, storage,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    VkDescriptorSetLayoutBinding bindings[std::size(types)]{};
    for (uint32_t i = 0; i < std::size(bindings); ++i) bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT};
    const VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr, 0, uint32_t(std::size(bindings)), bindings};
    Check(vk.vkCreateDescriptorSetLayout(device, &set_info, nullptr, &descriptors),
          "Create Vulkan temporal descriptor layout");
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    const VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr, 0, 1, &descriptors, 1, &range};
    Check(vk.vkCreatePipelineLayout(device, &layout_info, nullptr, &layout), "Create Vulkan temporal layout");

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kSlots},
                                          {storage, 6 * kSlots}, {sampled, 2 * kSlots}};
    const VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        nullptr, 0, kSlots, uint32_t(std::size(sizes)), sizes};
    Check(vk.vkCreateDescriptorPool(device, &pool_info, nullptr, &pool), "Create Vulkan temporal descriptor pool");
    std::array<VkDescriptorSetLayout, kSlots> layouts;
    layouts.fill(descriptors);
    const VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr, pool, kSlots, layouts.data()};
    Check(vk.vkAllocateDescriptorSets(device, &allocate, sets.data()), "Allocate Vulkan temporal descriptors");
    for (auto& counter : counters) {
      uint32_t memory_type = 0;
      if (!objects::CreateDedicatedAllocationBuffer(&vulkan, kParametersBytes,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              objects::MemoryPurpose::kReadback, counter.buffer, counter.memory, &memory_type)) {
        throw std::runtime_error("Allocate Vulkan frame coverage counters");
      }
      counter.coherent = (vulkan.GetMemoryTypes().host_coherent & (1u << memory_type)) != 0;
      void* mapped = nullptr;
      Check(vk.vkMapMemory(device, counter.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map Vulkan frame counters");
      counter.mapped = static_cast<const uint32_t*>(mapped);
    }

    constexpr VkFormat formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT};
    for (size_t i = 0; i < images.size(); ++i) {
      auto& image = images[i];
      auto& info = image.resource.description;
      info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      const auto extent = i == Output ? kOutput : kRender;
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = formats[i];
      info.extent = {extent.width, extent.height, 1};
      info.mipLevels = info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      if (!objects::CreateDedicatedAllocationImage(&vulkan, info, objects::MemoryPurpose::kDeviceLocal,
                                                    image.resource.image, image.memory)) {
        throw std::runtime_error("Allocate Vulkan temporal image " + std::to_string(i));
      }
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = image.resource.image;
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = info.format;
      view.subresourceRange = kColorRange;
      Check(vk.vkCreateImageView(device, &view, nullptr, &image.resource.view), "Create Vulkan temporal image view");
    }

    const std::span<const uint32_t> shaders[] = {kTemporalUnpack, kTemporalMotion,
                                                 kTemporalCompose, kTemporalHistory};
    for (size_t i = 0; i < pipelines.size(); ++i) {
      pipelines[i] = objects::CreateComputePipeline(&vulkan, layout, shaders[i].data(), shaders[i].size_bytes());
      if (!pipelines[i]) throw std::runtime_error("Create Vulkan temporal pipeline " + std::to_string(i));
    }
  }

  void Barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after,
               VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = kColorRange;
    vk.vkCmdPipelineBarrier(command, from, to, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  void Transition(VkCommandBuffer command, Image index, VkImageLayout after,
                  VkAccessFlags src, VkAccessFlags dst) {
    auto& image = images[index];
    const bool fresh = image.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    Barrier(command, image.resource.image, image.layout, after,
            fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : kCompute, kCompute, fresh ? 0 : src, dst);
    image.layout = after;
  }

  void Dispatch(VkCommandBuffer command, Pass pass, VkDescriptorSet set,
                const PushConstants& constants, VkExtent2D size) {
    vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass]);
    vk.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vk.vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vk.vkCmdDispatch(command, (size.width + 7) / 8, (size.height + 7) / 8, 1);
  }

  void CounterBarrier(VkCommandBuffer command, VkBuffer buffer,
                      VkPipelineStageFlags from, VkPipelineStageFlags to,
                      VkAccessFlags src, VkAccessFlags dst) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.size = kParametersBytes;
    vk.vkCmdPipelineBarrier(command, from, to, 0, 0, nullptr, 1, &barrier, 0, nullptr);
  }

  void ReportCounter(CounterBuffer& counter, std::ostream& log) {
    if (!counter.coherent) {
      const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr,
                                      counter.memory, 0, VK_WHOLE_SIZE};
      if (vk.vkInvalidateMappedMemoryRanges(device, 1, &range) != VK_SUCCESS) return;
    }
    const auto* values = counter.mapped;
    log << "coverage\t" << counter.frame << "\tscene=" << values[0]
        << "\toverlay=" << values[1] << "\tfallback=" << values[2]
        << "\tmotion=" << values[3] << std::endl;
  }

  void SetDescriptors(size_t slot, VkImageView guest, VkImageView output) {
    const auto set = sets[slot];
    const auto view = [&](Image image) { return images[image].resource.view; };
    const VkDescriptorBufferInfo buffer{inputs, 0, kInputsSize};
    const VkDescriptorBufferInfo counter{counters[slot].buffer, 0, kParametersBytes};
    const VkDescriptorImageInfo image_bindings[] = {
        {nullptr, view(Color), kGeneral}, {nullptr, view(Depth), kGeneral},
        {nullptr, view(Motion), kGeneral}, {nullptr, view(History), kGeneral},
        {sampler, view(Output), kGeneral}, {sampler, guest, kReadOnly}, {nullptr, output, kGeneral},
        {nullptr, view(Reactive), kGeneral}};
    VkWriteDescriptorSet writes[10]{};
    for (uint32_t i = 0; i < std::size(writes); ++i) {
      auto& w = writes[i];
      w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = i;
      w.descriptorCount = 1;
      w.descriptorType = i == 0 || i == 9 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
          i == 5 || i == 6 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w.pBufferInfo = i == 0 ? &buffer : i == 9 ? &counter : nullptr;
      w.pImageInfo = w.pBufferInfo ? nullptr : &image_bindings[i - 1];
    }
    vk.vkUpdateDescriptorSets(device, uint32_t(std::size(writes)), writes, 0, nullptr);
  }

  struct Pending {
    VkImage image;
    VkDescriptorSet set;
    size_t slot;
    uint64_t frame;
    std::array<int32_t, 2> allocation;
    bool written, swap_red_blue;
    std::array<uint32_t, 256> gamma;
  };

  const VulkanCommandProcessor& processor;
  const VulkanDevice& vulkan;
  const VulkanDevice::Functions& vk;
  VkDevice device;
  PFN_vkCmdUpdateBuffer update_buffer = nullptr;
  VkBuffer inputs = VK_NULL_HANDLE;
  VkDeviceMemory input_memory = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlots> sets{};
  std::array<uint64_t, kSlots> last_use{};
  std::array<CounterBuffer, kSlots> counters{};
  std::array<OwnedImage, Count> images{};
  std::array<VkPipeline, 4> pipelines{};
  std::optional<Pending> pending;
  uint64_t previous_frame = UINT64_MAX;
  Matrix previous_projection{};
  std::array<float, 2> previous_jitter{};
  Clock::time_point previous_time{};
  bool had_camera = false;
};

Temporal::Temporal(const VulkanCommandProcessor& processor) : state_(std::make_unique<State>(processor)) {
  state_->Init();
}

Temporal::~Temporal() = default;

VkBuffer Temporal::Inputs() const { return state_->inputs; }

bool Temporal::Schedule(uint64_t frame, VkImage image, VkImageView output, bool written, VkImageView guest,
                        uint32_t guest_width, uint32_t guest_height, bool swap_red_blue,
                        const std::array<uint32_t, 256>& gamma, std::ostream& log) {
  auto& s = *state_;
  const auto done = s.processor.GetCompletedSubmission();
  const auto slot = std::find_if(s.last_use.begin(), s.last_use.end(), [done](uint64_t num) { return num <= done; });
  if (s.pending || slot == s.last_use.end()) return false;
  const auto index = size_t(slot - s.last_use.begin());
  auto& counter = s.counters[index];
  if (*slot) s.ReportCounter(counter, log);
  counter.frame = frame;
  s.SetDescriptors(index, guest, output);
  *slot = s.processor.GetCurrentSubmission();
  s.pending = State::Pending{image, s.sets[index], index, frame, {int32_t(guest_width), int32_t(guest_height)},
                             written, swap_red_blue, gamma};
  return true;
}

bool Temporal::Record(VkCommandBuffer command, DlssVulkan* dlss, FsrVulkan* fsr,
                      const Matrix& projection, bool camera_found, std::array<float, 2> jitter,
                      uint32_t motion_draws, uint32_t hud_draws, uint32_t hud_blends,
                      uint32_t stencil_draws, std::ostream& log) {
  auto& s = *state_;
  if (!s.pending) return false;
  const auto pending = *std::exchange(s.pending, std::nullopt);
  const auto counter = s.counters[pending.slot].buffer;
  const uint32_t zero[4]{};
  s.update_buffer(command, counter, 0, sizeof(zero), zero);
  s.update_buffer(command, counter, kCounterBytes, kParametersBytes - kCounterBytes, pending.gamma.data());
  s.CounterBarrier(command, counter, VK_PIPELINE_STAGE_TRANSFER_BIT, kCompute,
                   VK_ACCESS_TRANSFER_WRITE_BIT, kSampled | kWritten);
  const float camera_direction = projection[3] * s.previous_projection[3] +
      projection[7] * s.previous_projection[7] + projection[11] * s.previous_projection[11];
  const bool reset = s.previous_frame == UINT64_MAX || pending.frame != s.previous_frame + 1 ||
      (camera_found && (!s.had_camera || camera_direction < 0.92f));
  const auto camera = MakeCamera(projection, s.previous_projection, camera_found, reset || !s.had_camera);
  PushConstants constants{camera.clip_to_previous_clip, jitter, reset ? jitter : s.previous_jitter,
                          pending.allocation, (reset ? 1u : 0u) | (pending.swap_red_blue ? 2u : 0u)};
  for (const auto i : {State::Color, State::Depth, State::Motion, State::Reactive, State::Output}) {
    s.Transition(command, i, kGeneral, kSampled | kWritten, kWritten);
  }
  s.Transition(command, State::History, kGeneral, kSampled | kWritten, kSampled | kWritten);
  s.Dispatch(command, State::Unpack, pending.set, constants, kRender);
  for (const auto i : {State::Color, State::Depth}) s.Transition(command, i, kGeneral, kWritten, kSampled);
  s.Dispatch(command, State::EstimateMotion, pending.set, constants, {kRender.width / 2, kRender.height / 2});
  for (const auto i : {State::Color, State::Depth, State::Motion, State::Reactive}) {
    s.Transition(command, i, kReadOnly, kSampled | kWritten, kSampled);
  }

  const auto now = Clock::now();
  VulkanUpscaleFrame frame{s.images[State::Color].resource, s.images[State::Depth].resource,
      s.images[State::Motion].resource, s.images[State::Reactive].resource, s.images[State::Output].resource};
  frame.jitter_x = jitter[0];
  frame.jitter_y = jitter[1];
  frame.reset_history = reset;
  frame.near_plane = camera.near_plane;
  frame.far_plane = camera.far_plane;
  frame.vertical_fov = camera.vertical_fov;
  frame.elapsed_ms = reset ? 1000.0f / 30 : std::clamp(
      std::chrono::duration<float, std::milli>(now - s.previous_time).count(), 1.0f, 100.0f);
  bool upscaled = false;
  try {
    if (dlss) dlss->Dispatch(command, frame, camera);
    else if (fsr) fsr->Dispatch(command, frame);
    else throw std::runtime_error("No Vulkan temporal provider is active");
    upscaled = true;
  } catch (const std::exception& failure) {
    log << "upscale-error\t" << pending.frame << '\t' << failure.what() << std::endl;
  }
  s.Transition(command, State::Color, kGeneral, kSampled, kSampled);
  if (upscaled) {
    s.Transition(command, State::Output, kGeneral, kWritten, kSampled);
    constants.flags |= 4u;
  }
  s.Barrier(command, pending.image, pending.written ? kReadOnly : VK_IMAGE_LAYOUT_UNDEFINED, kGeneral,
            pending.written ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            kCompute, pending.written ? kSampled : 0, kWritten);
  s.Dispatch(command, State::Compose, pending.set, constants, kOutput);
  s.Barrier(command, pending.image, kGeneral, kReadOnly, kCompute, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            kWritten, kSampled);
  s.Transition(command, State::History, kGeneral, kSampled, kWritten);
  s.Dispatch(command, State::StoreHistory, pending.set, constants, kRender);
  s.CounterBarrier(command, counter, kCompute, VK_PIPELINE_STAGE_HOST_BIT,
                   kWritten, VK_ACCESS_HOST_READ_BIT);
  s.previous_frame = upscaled ? pending.frame : UINT64_MAX;
  s.previous_time = now;
  s.previous_projection = projection;
  s.had_camera = camera_found;
  s.previous_jitter = jitter;
  if (upscaled) {
    log << "upscaled\t" << pending.frame << '\t' << (dlss ? "dlss" : "fsr3")
        << "\t2560x1440\thud=" << hud_draws << "\tblend=" << hud_blends
        << "\tstencil=" << stencil_draws << "\tguest_motion_draws=" << motion_draws
        << "\tcamera=" << camera_found << "\treset=" << reset << std::endl;
  }
  return upscaled;
}

}  // namespace wrathfall::vulkan_live
