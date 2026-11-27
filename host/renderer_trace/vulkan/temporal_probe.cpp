#include "fsr_vulkan.h"
#include "dlss_vulkan.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
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
  std::array<VulkanUpscaleImage, 4> images{};
  std::array<VkDeviceMemory, 4> memory{};
  VkBuffer readback = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;

#define FUNCTION(name) PFN_##name name = nullptr
  FUNCTION(vkGetDeviceProcAddr);
  FUNCTION(vkDestroyInstance);
  FUNCTION(vkDestroyDevice);
  FUNCTION(vkDeviceWaitIdle);
  FUNCTION(vkDestroyCommandPool);
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

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto provider = argc == 3 ? std::wstring_view(argv[1]) : std::wstring_view{};
  if (provider != L"fsr3" && provider != L"dlss") {
    std::fprintf(stderr, "Usage: temporal_probe <fsr3|dlss> <vendor runtime directory>\n");
    return 2;
  }
  try {
    Probe probe;
    const std::filesystem::path runtime(argv[2]);
    probe.Initialize(provider == L"dlss" ? runtime : std::filesystem::path{});
    probe.Run(runtime);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
