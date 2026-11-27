#include "fsr_vulkan.h"
#include "dlss_vulkan.h"
#include "temporal_history.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

using wrathfall::VulkanUpscaleImage;

namespace {

void Check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ")");
  }
}

template <typename Function, typename Pointer>
Function Require(Pointer address, const char* name) {
  if (!address) throw std::runtime_error(std::string("Missing Vulkan function ") + name);
  return reinterpret_cast<Function>(address);
}

struct Probe {
  ~Probe() {
    if (device) {
      vkDeviceWaitIdle(device);
      dlss.reset();
      if (pool) vkDestroyCommandPool(device, pool, nullptr);
      if (motion_pipeline) vkDestroyPipeline(device, motion_pipeline, nullptr);
      if (motion_pool) vkDestroyDescriptorPool(device, motion_pool, nullptr);
      if (motion_layout) vkDestroyPipelineLayout(device, motion_layout, nullptr);
      if (motion_descriptors) vkDestroyDescriptorSetLayout(device, motion_descriptors, nullptr);
      for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].view) vkDestroyImageView(device, images[i].view, nullptr);
        if (images[i].image) vkDestroyImage(device, images[i].image, nullptr);
        if (memory[i]) vkFreeMemory(device, memory[i], nullptr);
      }
      if (readback) vkDestroyBuffer(device, readback, nullptr);
      if (readback_memory) vkFreeMemory(device, readback_memory, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    if (loader) FreeLibrary(loader);
  }

  void Initialize(const std::filesystem::path& dlss_runtime);
  VulkanUpscaleImage CreateImage(size_t index, VkExtent2D extent, VkFormat format,
                                VkImageUsageFlags usage);
  void Clear(const VulkanUpscaleImage& image, const VkClearColorValue& color);
  void Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
               VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst);
  VkDeviceMemory Allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags);
  void Run(const std::filesystem::path& runtime);
  void RunMotionBaseline(const std::filesystem::path& shader);
  VkDescriptorSet CreateMotionPipeline(const std::filesystem::path& shader, VkDeviceSize stats_size);

  HMODULE loader = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties{};
  uint32_t family = 0;
  std::unique_ptr<wrathfall::DlssVulkan> dlss;
  std::array<VulkanUpscaleImage, 5> images{};
  std::array<VkDeviceMemory, 5> memory{};
  VkBuffer readback = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  VkPipeline motion_pipeline = VK_NULL_HANDLE;
  VkDescriptorPool motion_pool = VK_NULL_HANDLE;
  VkPipelineLayout motion_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout motion_descriptors = VK_NULL_HANDLE;

#define FUNCTION(name) PFN_##name name = nullptr
  FUNCTION(vkGetDeviceProcAddr);
  FUNCTION(vkDestroyInstance);
  FUNCTION(vkDestroyDevice);
  FUNCTION(vkDeviceWaitIdle);
  FUNCTION(vkDestroyCommandPool);
  FUNCTION(vkDestroyPipeline);
  FUNCTION(vkDestroyDescriptorPool);
  FUNCTION(vkDestroyPipelineLayout);
  FUNCTION(vkDestroyDescriptorSetLayout);
  FUNCTION(vkCreateImage);
  FUNCTION(vkDestroyImage);
  FUNCTION(vkCreateImageView);
  FUNCTION(vkDestroyImageView);
  FUNCTION(vkGetImageMemoryRequirements);
  FUNCTION(vkAllocateMemory);
  FUNCTION(vkFreeMemory);
  FUNCTION(vkBindImageMemory);
  FUNCTION(vkCmdPipelineBarrier);
  FUNCTION(vkCmdClearColorImage);
  FUNCTION(vkCreateBuffer);
  FUNCTION(vkDestroyBuffer);
  FUNCTION(vkGetBufferMemoryRequirements);
  FUNCTION(vkBindBufferMemory);
  FUNCTION(vkCmdCopyImageToBuffer);
  FUNCTION(vkBeginCommandBuffer);
  FUNCTION(vkEndCommandBuffer);
  FUNCTION(vkQueueSubmit);
  FUNCTION(vkQueueWaitIdle);
  FUNCTION(vkMapMemory);
  FUNCTION(vkUnmapMemory);
#undef FUNCTION
};

void Probe::Initialize(const std::filesystem::path& dlss_runtime) {
  if (!dlss_runtime.empty()) {
    std::puts("Loading Vulkan Streamline");
    dlss = std::make_unique<wrathfall::DlssVulkan>(dlss_runtime);
  }
  loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!loader) throw std::runtime_error("Vulkan loader unavailable");
  const auto get_instance_proc = Require<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(loader, "vkGetInstanceProcAddr"), "vkGetInstanceProcAddr");
  const auto vkCreateInstance =
      Require<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"), "vkCreateInstance");
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.pApplicationName = "WrathFall Vulkan upscaler probe";
  application.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &application;
  if (dlss) {
    const auto& extensions = dlss->InstanceExtensions();
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
  }
  Check(vkCreateInstance(&instance_info, nullptr, &instance), "Create Vulkan instance");

#define LOAD_INSTANCE(name) Require<PFN_##name>(get_instance_proc(instance, #name), #name)
  vkDestroyInstance = LOAD_INSTANCE(vkDestroyInstance);
  vkGetDeviceProcAddr = LOAD_INSTANCE(vkGetDeviceProcAddr);
  const auto enumerate = LOAD_INSTANCE(vkEnumeratePhysicalDevices);
  const auto properties = LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
  const auto queues = LOAD_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
  const auto features = LOAD_INSTANCE(vkGetPhysicalDeviceFeatures2);
  const auto memory_info = LOAD_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
  const auto create_device = LOAD_INSTANCE(vkCreateDevice);
#undef LOAD_INSTANCE

  uint32_t device_count = 0;
  Check(enumerate(instance, &device_count, nullptr), "Enumerate Vulkan adapters");
  std::vector<VkPhysicalDevice> devices(device_count);
  Check(enumerate(instance, &device_count, devices.data()), "Read Vulkan adapters");
  int best_rank = 0;
  for (auto candidate : devices) {
    VkPhysicalDeviceProperties info{};
    properties(candidate, &info);
    if (info.apiVersion < VK_API_VERSION_1_3 || info.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
    const int rank = info.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
    if (rank <= best_rank) continue;
    uint32_t count = 0;
    queues(candidate, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    queues(candidate, &count, families.data());
    constexpr VkQueueFlags both = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    const auto selected = std::find_if(families.begin(), families.end(), [both](const auto& item) {
      return item.queueCount && (item.queueFlags & both) == both;
    });
    if (selected == families.end()) continue;
    physical = candidate;
    family = static_cast<uint32_t>(selected - families.begin());
    best_rank = rank;
  }
  if (!physical) throw std::runtime_error("No Vulkan 1.3 GPU with a graphics/compute queue");
  VkPhysicalDeviceProperties info{};
  properties(physical, &info);
  std::printf("Adapter %s\n", info.deviceName);
  memory_info(physical, &memory_properties);

  VkPhysicalDeviceVulkan13Features core13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features core12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &core13};
  VkPhysicalDeviceVulkan11Features core11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &core12};
  VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &core11};
  features(physical, &supported);

  const float priority = 1;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.pNext = &supported;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  if (dlss) {
    dlss->CheckDevice(physical, core12, core13);
    const auto& extensions = dlss->DeviceExtensions();
    device_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    device_info.ppEnabledExtensionNames = extensions.data();
  }
  Check(create_device(physical, &device_info, nullptr, &device), "Create Vulkan device");

#define LOAD_DEVICE(name) Require<PFN_##name>(vkGetDeviceProcAddr(device, #name), #name)
#define LOAD(name) name = LOAD_DEVICE(name)
  LOAD(vkDestroyDevice);
  LOAD(vkDeviceWaitIdle);
  LOAD(vkDestroyCommandPool);
  LOAD(vkDestroyPipeline);
  LOAD(vkDestroyDescriptorPool);
  LOAD(vkDestroyPipelineLayout);
  LOAD(vkDestroyDescriptorSetLayout);
  LOAD(vkCreateImage);
  LOAD(vkDestroyImage);
  LOAD(vkCreateImageView);
  LOAD(vkDestroyImageView);
  LOAD(vkGetImageMemoryRequirements);
  LOAD(vkAllocateMemory);
  LOAD(vkFreeMemory);
  LOAD(vkBindImageMemory);
  LOAD(vkCmdPipelineBarrier);
  LOAD(vkCmdClearColorImage);
  LOAD(vkCreateBuffer);
  LOAD(vkDestroyBuffer);
  LOAD(vkGetBufferMemoryRequirements);
  LOAD(vkBindBufferMemory);
  LOAD(vkCmdCopyImageToBuffer);
  LOAD(vkBeginCommandBuffer);
  LOAD(vkEndCommandBuffer);
  LOAD(vkQueueSubmit);
  LOAD(vkQueueWaitIdle);
  LOAD(vkMapMemory);
  LOAD(vkUnmapMemory);
  LOAD_DEVICE(vkGetDeviceQueue)(device, family, 0, &queue);
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = family;
  Check(LOAD_DEVICE(vkCreateCommandPool)(device, &pool_info, nullptr, &pool), "Create command pool");
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  Check(LOAD_DEVICE(vkAllocateCommandBuffers)(device, &allocate, &command), "Allocate command buffer");
#undef LOAD
#undef LOAD_DEVICE
}

VkDeviceMemory Probe::Allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
    if (!(requirements.memoryTypeBits & (1u << i))) continue;
    if ((memory_properties.memoryTypes[i].propertyFlags & flags) != flags) continue;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.memoryTypeIndex = i;
    allocation.allocationSize = requirements.size;
    VkDeviceMemory allocation_memory = VK_NULL_HANDLE;
    Check(vkAllocateMemory(device, &allocation, nullptr, &allocation_memory), "Allocate Vulkan memory");
    return allocation_memory;
  }
  throw std::runtime_error("No matching Vulkan memory type");
}

constexpr VkImageSubresourceRange kColor{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

VulkanUpscaleImage Probe::CreateImage(size_t index, VkExtent2D extent, VkFormat format,
                                     VkImageUsageFlags usage) {
  auto& image = images[index];
  image.description = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  auto& description = image.description;
  description.imageType = VK_IMAGE_TYPE_2D;
  description.format = format;
  description.extent = {extent.width, extent.height, 1};
  description.mipLevels = 1;
  description.arrayLayers = 1;
  description.samples = VK_SAMPLE_COUNT_1_BIT;
  description.usage = usage;
  Check(vkCreateImage(device, &description, nullptr, &image.image), "Create test image");
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device, image.image, &requirements);
  memory[index] = Allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  Check(vkBindImageMemory(device, image.image, memory[index], 0), "Bind test image memory");
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = kColor;
  Check(vkCreateImageView(device, &view, nullptr, &image.view), "Create test image view");
  return image;
}

void Probe::Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = kColor;
  vkCmdPipelineBarrier(command, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void Probe::Clear(const VulkanUpscaleImage& image, const VkClearColorValue& color) {
  Barrier(image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdClearColorImage(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &kColor);
  Barrier(image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void Probe::Run(const std::filesystem::path& runtime) {
  constexpr VkExtent2D render{1280, 720}, output{2560, 1440};
  constexpr auto input_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  wrathfall::VulkanUpscaleFrame frame;
  frame.color = CreateImage(0, render, VK_FORMAT_R8G8B8A8_UNORM, input_usage);
  frame.depth = CreateImage(1, render, VK_FORMAT_R32_SFLOAT, input_usage);
  frame.motion = CreateImage(2, render, VK_FORMAT_R16G16_SFLOAT, input_usage);
  frame.output = CreateImage(3, output, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = static_cast<VkDeviceSize>(output.width) * output.height * 4;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  Check(vkCreateBuffer(device, &buffer_info, nullptr, &readback), "Create readback buffer");
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, readback, &requirements);
  readback_memory = Allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  Check(vkBindBufferMemory(device, readback, readback_memory, 0), "Bind readback buffer");

  std::unique_ptr<wrathfall::FsrVulkan> fsr;
  if (dlss) {
    std::puts("Binding Streamline to the Vulkan device");
    dlss->Bind(instance, physical, device, family, render, output, asura::temporal::DlssPreset::K, false);
    std::puts("Provider DLSS, preset K");
  } else {
    fsr = std::make_unique<wrathfall::FsrVulkan>(runtime, physical, device, vkGetDeviceProcAddr,
                                               render, output, false);
    const auto provider = fsr->Provider();
    std::printf("Provider FSR %.*s\n", static_cast<int>(provider.size()), provider.data());
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Check(vkBeginCommandBuffer(command, &begin), "Begin FSR test commands");
  Clear(frame.color, {{0.2f, 0.6f, 0.9f, 1.0f}});
  Clear(frame.depth, {{0.5f, 0, 0, 0}});
  Clear(frame.motion, {{0, 0, 0, 0}});
  Barrier(frame.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  if (dlss) {
    std::puts("Recording Vulkan DLSS commands");
    constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const asura::temporal::Camera camera{identity, identity, identity, identity, {0, 0, 0},
                                         {0, 1, 0}, {1, 0, 0}, {0, 0, 1},
                                         frame.near_plane, frame.far_plane, frame.vertical_fov, 1};
    dlss->Dispatch(command, frame, camera);
  } else {
    fsr->Dispatch(command, frame);
  }
  Barrier(frame.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {output.width, output.height, 1};
  vkCmdCopyImageToBuffer(command, frame.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
  Check(vkEndCommandBuffer(command), "End FSR test commands");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "Submit FSR test commands");
  Check(vkQueueWaitIdle(queue), "Wait for FSR output");

  void* mapped = nullptr;
  Check(vkMapMemory(device, readback_memory, 0, buffer_info.size, 0, &mapped), "Read upscaled image");
  const auto* pixels = static_cast<const uint8_t*>(mapped);
  uint64_t red = 0, green = 0, blue = 0;
  for (VkDeviceSize i = 0; i < buffer_info.size; i += 4) {
    red += pixels[i];
    green += pixels[i + 1];
    blue += pixels[i + 2];
  }
  vkUnmapMemory(device, readback_memory);
  const auto count = static_cast<double>(output.width) * output.height;
  std::printf("1280x720 -> 2560x1440 readback RGB means %.2f %.2f %.2f\n",
              red / count, green / count, blue / count);
  if (red / count < 30 || green / count < 100 || blue / count < 170 || !(red < green && green < blue)) {
    throw std::runtime_error("Vulkan output did not reproduce the test color");
  }
}

namespace motion_test {

constexpr VkExtent2D kRender{1280, 720};
struct Constants {
  std::array<float, 16> warp{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::array<float, 4> jitter{};
  std::array<int32_t, 2> extent{1280, 720};
  uint32_t flags = 0;
};
static_assert(sizeof(Constants) == 92);

using Pixel = std::array<uint16_t, 4>;
Pixel Texture(int x, int y) {
  uint32_t hash = uint32_t(x) * 0x9e3779b1u ^ uint32_t(y) * 0x85ebca6bu;
  hash = (hash ^ (hash >> 16)) * 0x7feb352du;
  // Bit patterns represent a deterministic range of positive half-float values.
  const auto value = static_cast<uint16_t>(0x3000u + ((hash ^ (hash >> 15)) & 0x7ffu));
  return {value, value, value, 0x3c00};
}

VkBufferImageCopy ImageCopy(VkDeviceSize offset) {
  VkBufferImageCopy copy{};
  copy.bufferOffset = offset;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {kRender.width, kRender.height, 1};
  return copy;
}

}  // namespace motion_test

#define LOAD_MOTION(name) const auto name = Require<PFN_##name>(vkGetDeviceProcAddr(device, #name), #name)

VkDescriptorSet Probe::CreateMotionPipeline(const std::filesystem::path& shader, VkDeviceSize stats_size) {
  std::ifstream file(shader, std::ios::binary | std::ios::ate);
  const std::streamoff shader_bytes = file.tellg();
  if (shader_bytes < 20 || shader_bytes % 4 || shader_bytes > 4 * 1024 * 1024) {
    throw std::runtime_error("Invalid Vulkan motion SPIR-V file");
  }
  std::vector<uint32_t> code(static_cast<size_t>(shader_bytes) / sizeof(uint32_t));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(code.data()), shader_bytes) || code[0] != 0x07230203) {
    throw std::runtime_error("Could not read Vulkan motion SPIR-V");
  }
  LOAD_MOTION(vkCreateDescriptorSetLayout);
  LOAD_MOTION(vkCreateDescriptorPool);
  LOAD_MOTION(vkAllocateDescriptorSets);
  LOAD_MOTION(vkUpdateDescriptorSets);
  LOAD_MOTION(vkCreatePipelineLayout);
  LOAD_MOTION(vkCreateShaderModule);
  LOAD_MOTION(vkDestroyShaderModule);
  LOAD_MOTION(vkCreateComputePipelines);
  constexpr auto storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  constexpr auto buffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  const uint32_t bindings[] = {1, 2, 3, 4, 8, 9};
  VkDescriptorSetLayoutBinding layout_bindings[std::size(bindings)]{};
  for (size_t i = 0; i < std::size(bindings); ++i) {
    layout_bindings[i] = {bindings[i], i == images.size() ? buffer : storage, 1, VK_SHADER_STAGE_COMPUTE_BIT};
  }
  const VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      nullptr, 0, uint32_t(std::size(bindings)), layout_bindings};
  Check(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &motion_descriptors), "Create motion descriptor layout");
  const VkDescriptorPoolSize sizes[] = {{storage, uint32_t(images.size())}, {buffer, 1}};
  const VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      nullptr, 0, 1, uint32_t(std::size(sizes)), sizes};
  Check(vkCreateDescriptorPool(device, &pool_info, nullptr, &motion_pool), "Create motion descriptor pool");
  const VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      nullptr, motion_pool, 1, &motion_descriptors};
  VkDescriptorSet set = VK_NULL_HANDLE;
  Check(vkAllocateDescriptorSets(device, &set_info, &set), "Allocate motion descriptor set");
  const VkDescriptorBufferInfo stats{readback, 0, stats_size};
  std::array<VkDescriptorImageInfo, 5> views{};
  VkWriteDescriptorSet writes[std::size(bindings)]{};
  for (size_t i = 0; i < images.size(); ++i) {
    views[i] = {nullptr, images[i].view, VK_IMAGE_LAYOUT_GENERAL};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, bindings[i], 0, 1, storage, &views[i]};
  }
  writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 9, 0, 1, buffer, nullptr, &stats};
  vkUpdateDescriptorSets(device, uint32_t(std::size(writes)), writes, 0, nullptr);

  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(motion_test::Constants)};
  const VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      nullptr, 0, 1, &motion_descriptors, 1, &range};
  Check(vkCreatePipelineLayout(device, &pipeline_layout, nullptr, &motion_layout), "Create motion pipeline layout");
  VkShaderModule module = VK_NULL_HANDLE;
  const VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      nullptr, 0, code.size() * sizeof(uint32_t), code.data()};
  Check(vkCreateShaderModule(device, &module_info, nullptr, &module), "Create motion shader module");
  VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
  pipeline.layout = motion_layout;
  const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &motion_pipeline);
  vkDestroyShaderModule(device, module, nullptr);
  Check(result, "Create motion compute pipeline");
  return set;
}

void Probe::RunMotionBaseline(const std::filesystem::path& shader) {
  using namespace motion_test;
  constexpr VkDeviceSize pixels = VkDeviceSize(kRender.width) * kRender.height;
  constexpr VkDeviceSize stats_size = 260 * sizeof(uint32_t);
  constexpr VkDeviceSize current_offset = stats_size;
  constexpr VkDeviceSize previous_offset = current_offset + pixels * sizeof(Pixel);
  constexpr VkDeviceSize motion_offset = previous_offset + pixels * sizeof(Pixel);
  constexpr VkDeviceSize reactive_offset = motion_offset + pixels * 2 * sizeof(float);
  constexpr VkDeviceSize bytes = reactive_offset + pixels * sizeof(float);
  constexpr auto compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  constexpr auto transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
  constexpr auto top = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  constexpr auto general = VK_IMAGE_LAYOUT_GENERAL;
  constexpr auto dst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  constexpr auto src = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  constexpr auto undefined = VK_IMAGE_LAYOUT_UNDEFINED;
  constexpr auto upload = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  constexpr auto download = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  const auto scene = CreateImage(0, kRender, VK_FORMAT_R16G16B16A16_SFLOAT, upload);
  const auto depth = CreateImage(1, kRender, VK_FORMAT_R32_SFLOAT, upload);
  const auto motion = CreateImage(2, kRender, VK_FORMAT_R32G32_SFLOAT, download);
  const auto previous = CreateImage(3, kRender, VK_FORMAT_R16G16B16A16_SFLOAT, upload);
  const auto reactive = CreateImage(4, kRender, VK_FORMAT_R32_SFLOAT, download);
  const VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, bytes,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
  Check(vkCreateBuffer(device, &buffer, nullptr, &readback), "Create motion test buffer");
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, readback, &requirements);
  readback_memory = Allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  Check(vkBindBufferMemory(device, readback, readback_memory, 0), "Bind motion test buffer");
  const VkDescriptorSet set = CreateMotionPipeline(shader, stats_size);
  LOAD_MOTION(vkCmdCopyBufferToImage);
  LOAD_MOTION(vkCmdFillBuffer);
  LOAD_MOTION(vkCmdBindPipeline);
  LOAD_MOTION(vkCmdBindDescriptorSets);
  LOAD_MOTION(vkCmdPushConstants);
  LOAD_MOTION(vkCmdDispatch);

  struct TestCase {
    const char* name;
    uint64_t frame, previous_frame;
    bool had_camera;
    float direction;
    int camera_x, camera_y, object_x, object_y;
    bool reset;
  };
  const TestCase tests[] = {
      {"first frame resets", 0, UINT64_MAX, false, 1, 8, -4, 4, 4, true},
      {"static scene", 1, 0, true, 1, 0, 0, 0, 0, false},
      {"known camera pan", 2, 1, true, .98f, 8, -4, 0, 0, false},
      {"camera and object motion", 3, 2, true, .98f, 8, -4, 4, 4, false},
      {"camera cut resets", 4, 3, true, .25f, 8, -4, 4, 4, true},
      {"dropped frame resets", 6, 4, true, 1, 8, -4, 4, 4, true},
      {"new camera resets", 7, 6, false, 1, 8, -4, 4, 4, true},
  };
  for (const auto& test : tests) {
    const bool reset = wrathfall::vulkan_live::ShouldResetHistory(test.frame, test.previous_frame, true,
                                                                 test.had_camera, test.direction);
    if (reset != test.reset) throw std::runtime_error(std::string("History decision: ") + test.name);
    Constants constants;
    constants.flags = reset ? 1u : 0u;
    constants.warp[12] = 2.0f * float(test.camera_x) / float(kRender.width);
    constants.warp[13] = -2.0f * float(test.camera_y) / float(kRender.height);

    void* mapped = nullptr;
    Check(vkMapMemory(device, readback_memory, 0, bytes, 0, &mapped), "Map motion inputs");
    auto* data = static_cast<uint8_t*>(mapped);
    auto* current = reinterpret_cast<Pixel*>(data + current_offset);
    auto* old = reinterpret_cast<Pixel*>(data + previous_offset);
    for (int y = 0; y < int(kRender.height); ++y) {
      for (int x = 0; x < int(kRender.width); ++x) {
        const auto i = size_t(y) * kRender.width + size_t(x);
        const bool object = x >= int(kRender.width / 2);
        current[i] = Texture(x + test.camera_x + (object ? test.object_x : 0),
                             y + test.camera_y + (object ? test.object_y : 0));
        old[i] = Texture(x, y);
      }
    }
    vkUnmapMemory(device, readback_memory);

    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    Check(vkBeginCommandBuffer(command, &begin), "Begin motion test commands");
    for (auto image : {scene.image, previous.image, depth.image}) {
      Barrier(image, undefined, dst, 0, VK_ACCESS_TRANSFER_WRITE_BIT, top, transfer);
    }
    const auto current_copy = ImageCopy(current_offset), previous_copy = ImageCopy(previous_offset);
    vkCmdCopyBufferToImage(command, readback, scene.image, dst, 1, &current_copy);
    vkCmdCopyBufferToImage(command, readback, previous.image, dst, 1, &previous_copy);
    const VkClearColorValue depth_value{{.5f, 0, 0, 0}};
    vkCmdClearColorImage(command, depth.image, dst, &depth_value, 1, &kColor);
    for (auto image : {scene.image, previous.image, depth.image}) {
      Barrier(image, dst, general, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, transfer, compute);
    }
    for (auto image : {motion.image, reactive.image}) {
      Barrier(image, undefined, general, 0, VK_ACCESS_SHADER_WRITE_BIT, top, compute);
    }
    vkCmdFillBuffer(command, readback, 0, stats_size, 0);
    const VkMemoryBarrier stats_ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command, transfer, compute, 0, 1, &stats_ready, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, motion_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, motion_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(command, motion_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(command, (kRender.width + 15) / 16, (kRender.height + 15) / 16, 1);
    for (auto image : {motion.image, reactive.image}) {
      Barrier(image, general, src, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, compute, transfer);
    }
    const auto motion_copy = ImageCopy(motion_offset), reactive_copy = ImageCopy(reactive_offset);
    vkCmdCopyImageToBuffer(command, motion.image, src, readback, 1, &motion_copy);
    vkCmdCopyImageToBuffer(command, reactive.image, src, readback, 1, &reactive_copy);
    const VkMemoryBarrier readback_ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, transfer, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &readback_ready, 0, nullptr, 0, nullptr);
    Check(vkEndCommandBuffer(command), "End motion test commands");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "Submit motion test commands");
    Check(vkQueueWaitIdle(queue), "Wait for motion output");

    Check(vkMapMemory(device, readback_memory, 0, bytes, 0, &mapped), "Read Vulkan motion output");
    data = static_cast<uint8_t*>(mapped);
    const auto* vectors = reinterpret_cast<const float*>(data + motion_offset);
    const auto* masks = reinterpret_cast<const float*>(data + reactive_offset);
    size_t matched = 0, samples = 0;
    for (int y = 24; y < int(kRender.height) - 24; y += 22) {
      for (int x = 24; x < int(kRender.width) - 24; x += 22) {
        if (std::abs(x - int(kRender.width / 2)) < 24) continue;
        const bool object = x >= int(kRender.width / 2);
        const auto i = size_t(y) * kRender.width + size_t(x);
        const float expected_x = reset ? 0 : float(test.camera_x + (object ? test.object_x : 0));
        const float expected_y = reset ? 0 : float(test.camera_y + (object ? test.object_y : 0));
        if (std::abs(vectors[2 * i] - expected_x) < .01f &&
            std::abs(vectors[2 * i + 1] - expected_y) < .01f && std::abs(masks[i]) < .01f) ++matched;
        ++samples;
      }
    }
    vkUnmapMemory(device, readback_memory);
    std::printf("%s: %zu/%zu vectors and reactive values matched\n", test.name, matched, samples);
    if (matched != samples) throw std::runtime_error(std::string("Motion baseline mismatch: ") + test.name);
  }
}

#undef LOAD_MOTION

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto provider = argc == 3 ? std::wstring_view(argv[1]) : std::wstring_view{};
  if (provider != L"fsr3" && provider != L"dlss" && provider != L"motion") {
    std::fprintf(stderr, "Usage: temporal_probe <fsr3|dlss> <vendor runtime directory>\n"
                         "       temporal_probe motion <live motion SPIR-V file>\n");
    return 2;
  }
  try {
    Probe probe;
    const std::filesystem::path runtime(argv[2]);
    probe.Initialize(provider == L"dlss" ? runtime : std::filesystem::path{});
    if (provider == L"motion") probe.RunMotionBaseline(runtime);
    else probe.Run(runtime);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
