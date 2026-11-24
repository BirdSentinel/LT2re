#include "render.hpp"
#include "performance.hpp"
#include "shaders.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <set>
#include <string>
namespace lt2 {
static void check(VkResult r, const char *where) {
  if (r != VK_SUCCESS) {
    char b[256];
    snprintf(b, sizeof b, "%s: Vulkan error %d", where, int(r));
    throw std::runtime_error(b);
  }
}
#define VK(call) check(call, #call)
bool Raster::operator<(const Raster &b) const {
  return memcmp(this, &b, sizeof b) < 0;
}
struct Buffer {
  VkBuffer handle{};
  VkDeviceMemory memory{};
  void *mapped{};
  VkDevice device{};
  void destroy() {
    if (mapped)
      vkUnmapMemory(device, memory);
    if (handle)
      vkDestroyBuffer(device, handle, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
    handle = {};
    memory = {};
    mapped = nullptr;
  }
};
struct Image : Texture {
  VkDeviceSize allocationBytes = 0;
  VkDevice device{};
  VkImage image{};
  VkDeviceMemory memory{};
  VkImageView view{};
  uint32_t levels = 1;
  ~Image() {
    if (view)
      vkDestroyImageView(device, view, nullptr);
    if (image)
      vkDestroyImage(device, image, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
  }
};
struct GeometryBuffer : Geometry {
  Buffer buffer;
  ~GeometryBuffer() { buffer.destroy(); }
};
struct Renderer::Impl {
  Platform &platform;
  VkInstance instance{};
  VkSurfaceKHR surface{};
  VkPhysicalDevice gpu{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkPhysicalDeviceProperties properties{};
  VkSwapchainKHR swap{};
  VkFormat format{},depthFormat=VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
  VkRenderPass pass{};
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<std::shared_ptr<Image>> depths;
  std::vector<VkFramebuffer> targets;
  std::vector<VkSemaphore> complete;
  VkDescriptorSetLayout setLayout{};
  VkPipelineLayout layout{};
  VkPipelineCache cache{};
  VkShaderModule vs[3]{}, fs{};
  std::map<Raster, VkPipeline> pipelines;
  std::map<std::array<uint32_t, 7>, VkSampler> samplers;
  struct Frame {
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkSemaphore acquired{};
    VkDescriptorPool descriptors{};
    Buffer upload;
    size_t used = 0;
    std::vector<std::shared_ptr<Texture>> retained;
    std::set<Texture *> retainedImages;
    std::vector<std::shared_ptr<Geometry>> geometry;
    std::map<std::array<uint64_t, 8>, VkDescriptorSet> textureSets;
    VkDeviceSize retainedBytes = 0;
  };
  Frame frames[2];
  uint32_t frameIndex = 0, imageIndex = 0;
  bool recording = false, dirty = true;
  const char *testCapture = getenv("LT2_VK_TEST_CAPTURE");
  std::shared_ptr<Texture> white;
  uint64_t presents = 0, draws = 0;
  std::chrono::steady_clock::time_point timer =
      std::chrono::steady_clock::now();
  explicit Impl(Platform &p) : platform(p) {}
  uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) &&
          (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No compatible GPU memory type");
  }
  Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    b.device = device;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VK(vkCreateBuffer(device, &ci, nullptr, &b.handle));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.handle, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkAllocateMemory(device, &ai, nullptr, &b.memory));
    VK(vkBindBufferMemory(device, b.handle, b.memory, 0));
    VK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
  }
  std::shared_ptr<Image> image(uint32_t w, uint32_t h, uint32_t levels,
                               VkFormat fmt, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect) {
    auto b = std::make_shared<Image>();
    b->device = device;
    b->levels = levels;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VK(vkCreateImage(device, &ci, nullptr, &b->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, b->image, &req);
    b->allocationBytes = req.size;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK(vkAllocateMemory(device, &ai, nullptr, &b->memory));
    VK(vkBindImageMemory(device, b->image, b->memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = b->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, levels, 0, 1};
    VK(vkCreateImageView(device, &vi, nullptr, &b->view));
    return b;
  }
  VkShaderModule shader(const uint32_t *data, size_t size) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = data;
    VkShaderModule m;
    VK(vkCreateShaderModule(device, &ci, nullptr, &m));
    return m;
  }
  void init() {
    if(auto loader=platform.loader())volkInitializeCustom(loader);else VK(volkInitialize());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "London Taxi 2 portable renderer";
    app.apiVersion = VK_API_VERSION_1_0;
    const char *extensions[] = {"VK_KHR_surface", platform.surfaceExtension()};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;
    const char *validation = "VK_LAYER_KHRONOS_validation";
    if (getenv("LT2_VK_VALIDATION")) {
      ci.enabledLayerCount = 1;
      ci.ppEnabledLayerNames = &validation;
    }
    VK(vkCreateInstance(&ci, nullptr, &instance));
    volkLoadInstance(instance);
    surface = platform.surface(instance);
    uint32_t count = 0;
    VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    int best = -1;
    for (auto d : devices) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(d, &props);
      uint32_t n = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &n, nullptr);
      std::vector<VkQueueFamilyProperties> qs(n);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &n, qs.data());
      for (uint32_t i = 0; i < n; ++i) {
        VkBool32 supported = 0;
        VK(vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &supported));
        int score =
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (supported && (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            score > best) {
          gpu = d;
          family = i;
          best = score;
        }
      }
    }
    if (!gpu)
      throw std::runtime_error("No Vulkan graphics/present queue");
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    vkGetPhysicalDeviceProperties(gpu, &properties);
    for(auto fmt:{VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_D32_SFLOAT_S8_UINT}){VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(gpu,fmt,&fp);if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){depthFormat=fmt;break;}}
    if(depthFormat==VK_FORMAT_UNDEFINED)throw std::runtime_error("No depth/stencil attachment format");
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char *ext = "VK_KHR_swapchain";
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = &ext;
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(gpu, &available);
    VkPhysicalDeviceFeatures enabled{};
    enabled.textureCompressionBC = available.textureCompressionBC;
    di.pEnabledFeatures = &enabled;
    VK(vkCreateDevice(gpu, &di, nullptr, &device));
    volkLoadDevice(device);
    vkGetDeviceQueue(device, family, 0, &queue);
    fprintf(stderr, "[vulkan] GPU=%s API=%u.%u custom portable renderer\n",
            properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
            VK_VERSION_MINOR(properties.apiVersion));
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo si{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 2;
    si.pBindings = bindings;
    VK(vkCreateDescriptorSetLayout(device, &si, nullptr, &setLayout));
    VkPipelineLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &setLayout;
    VK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VK(vkCreatePipelineCache(device, &pci, nullptr, &cache));
    vs[0] = shader(fixed_vert, sizeof fixed_vert);
    vs[1] = shader(world1_vert, sizeof world1_vert);
    vs[2] = shader(world2_vert, sizeof world2_vert);
    fs = shader(fixed_frag, sizeof fixed_frag);
    for (auto &f : frames) {
      VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      cp.queueFamilyIndex = family;
      cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      VK(vkCreateCommandPool(device, &cp, nullptr, &f.pool));
      VkCommandBufferAllocateInfo ca{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ca.commandPool = f.pool;
      ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ca.commandBufferCount = 1;
      VK(vkAllocateCommandBuffers(device, &ca, &f.cmd));
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      VK(vkCreateFence(device, &fi, nullptr, &f.fence));
      VkSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VK(vkCreateSemaphore(device, &se, nullptr, &f.acquired));
      VkDescriptorPoolSize sizes[] = {
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192},
          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}};
      VkDescriptorPoolCreateInfo dp{
          VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      dp.maxSets = 8192;
      dp.poolSizeCount = 2;
      dp.pPoolSizes = sizes;
      VK(vkCreateDescriptorPool(device, &dp, nullptr, &f.descriptors));
      f.upload =
          buffer(64 * 1024 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    }
    TextureData td{1, 1, 21, {{255, 255, 255, 255}}};
    white = upload(td);
  }
  void destroySwap() {
    for (auto t : targets)
      vkDestroyFramebuffer(device, t, nullptr);
    targets.clear();
    depths.clear();
    for (auto v : views)
      vkDestroyImageView(device, v, nullptr);
    views.clear();
    for (auto s : complete)
      vkDestroySemaphore(device, s, nullptr);
    complete.clear();
    if (swap)
      vkDestroySwapchainKHR(device, swap, nullptr);
    swap = {};
  }
  bool resize() {
    uint32_t w, h;
    platform.extent(w, h);
    if (!w || !h)
      return false;
    VK(vkDeviceWaitIdle(device));
    destroySwap();
    VkSurfaceCapabilitiesKHR caps;
    VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
      extent = {
          std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width),
          std::clamp(h, caps.minImageExtent.height,
                     caps.maxImageExtent.height)};
    uint32_t n = 0;
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(n);
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats.data()));
    if (!n)
      throw std::runtime_error("No surface formats");
    auto chosen = formats[0];
    for (auto f : formats)
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
        chosen = f;
    format = chosen.format;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount)
      ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(testCapture) {
      if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        throw std::runtime_error("Test capture transfer source unsupported");
      ci.imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (uint32_t a = 1; a <= 8; a <<= 1)
      if (caps.supportedCompositeAlpha & a) {
        ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)a;
        break;
      }
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (getenv("LT2_VK_UNCAPPED") && strcmp(getenv("LT2_VK_UNCAPPED"), "0") != 0) {
      uint32_t m = 0;
      vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &m, nullptr);
      std::vector<VkPresentModeKHR> modes(m);
      vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &m, modes.data());
      for (auto mode : modes)
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
          ci.presentMode = mode;
    }
    ci.clipped = VK_TRUE;
    VK(vkCreateSwapchainKHR(device, &ci, nullptr, &swap));
    VK(vkGetSwapchainImagesKHR(device, swap, &n, nullptr));
    images.resize(n);
    VK(vkGetSwapchainImagesKHR(device, swap, &n, images.data()));
    if (!pass) {
      VkAttachmentDescription attachments[2]{};
      attachments[0].format = format;
      attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
      attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      attachments[1].format = depthFormat;
      attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
      attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachments[1].finalLayout =
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      VkAttachmentReference col{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
          dep{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      VkSubpassDescription sub{};
      sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub.colorAttachmentCount = 1;
      sub.pColorAttachments = &col;
      sub.pDepthStencilAttachment = &dep;
      VkSubpassDependency dependency{};
      dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
      dependency.dstSubpass = 0;
      dependency.srcStageMask = dependency.dstStageMask =
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
      dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
      rp.attachmentCount = 2;
      rp.pAttachments = attachments;
      rp.subpassCount = 1;
      rp.pSubpasses = &sub;
      rp.dependencyCount = 1;
      rp.pDependencies = &dependency;
      VK(vkCreateRenderPass(device, &rp, nullptr, &pass));
    }
    for (auto im : images) {
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = im;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view;
      VK(vkCreateImageView(device, &vi, nullptr, &view));
      views.push_back(view);
      auto depth =
          image(extent.width, extent.height, 1, depthFormat,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
      depths.push_back(depth);
      VkImageView ats[] = {view, depth->view};
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fi.renderPass = pass;
      fi.attachmentCount = 2;
      fi.pAttachments = ats;
      fi.width = extent.width;
      fi.height = extent.height;
      fi.layers = 1;
      VkFramebuffer fb;
      VK(vkCreateFramebuffer(device, &fi, nullptr, &fb));
      targets.push_back(fb);
      VkSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore sem;
      VK(vkCreateSemaphore(device, &se, nullptr, &sem));
      complete.push_back(sem);
    }
    dirty = false;
    fprintf(stderr, "[vulkan] swapchain %ux%u images=%u present=%d\n",
            extent.width, extent.height, n, int(ci.presentMode));
    return true;
  }
  bool begin() {
    if (recording)
      return true;
    uint32_t w, h;
    platform.extent(w, h);
    if (!w || !h)
      return false;
    if (w != extent.width || h != extent.height)
      dirty = true;
    if (dirty && !resize())
      return false;
    auto &f = frames[frameIndex];
    VkResult r;
    {
      PerfScope timing(performance.wait);
      VK(vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX));
      r = vkAcquireNextImageKHR(device, swap, UINT64_MAX, f.acquired,
                               VK_NULL_HANDLE, &imageIndex);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      dirty = true;
      return false;
    }
    if (r != VK_SUBOPTIMAL_KHR)
      VK(r);
    else
      dirty = true;
    // Descriptor sets reference this frame's stable upload buffer at offset 0.
    // Keep immutable texture combinations across frames; dynamic offsets select
    // the current uniforms. Images stay alive with the cache. Bound retention.
    if (f.textureSets.size() >= 1024 || f.retainedBytes >= 64*1024*1024) {
      f.textureSets.clear();
      f.retained.clear();
      f.retainedImages.clear();
      f.retainedBytes = 0;
      VK(vkResetDescriptorPool(device, f.descriptors, 0));
    }
    f.used = 0;
    f.geometry.clear(); // this frame's fence has completed
    VK(vkResetCommandPool(device, f.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(f.cmd, &bi));
    VkClearValue clears[2]{};
    clears[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    ri.renderPass = pass;
    ri.framebuffer = targets[imageIndex];
    ri.renderArea.extent = extent;
    ri.clearValueCount = 2;
    ri.pClearValues = clears;
    vkCmdBeginRenderPass(f.cmd, &ri, VK_SUBPASS_CONTENTS_INLINE);
    recording = true;
    return true;
  }
  size_t push(const void *data, size_t size, size_t alignment) {
    auto &f = frames[frameIndex];
    size_t off = (f.used + alignment - 1) & ~(alignment - 1);
    if (off + size > 64 * 1024 * 1024)
      throw std::runtime_error("Frame upload arena exhausted (64 MiB)");
    memcpy((char *)f.upload.mapped + off, data, size);
    performance.uploadBytes += size;
    f.used = off + size;
    return off;
  }
  VkPipeline pipeline(const Raster &r) {
    auto found = pipelines.find(r);
    if (found != pipelines.end())
      return found->second;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto &s : stages) {
      s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      s.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs[r.shader];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    VkVertexInputBindingDescription bindings[] = {
      {0, r.input.stride, VK_VERTEX_INPUT_RATE_VERTEX},
      {1, 0, VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attrs[9];
    const VkFormat formats[] = {VK_FORMAT_R32G32B32A32_SFLOAT,
      VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32B32_SFLOAT,
      VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R8G8B8A8_UNORM};
    for (uint32_t i = 0; i < 9; ++i) {
      uint32_t fmt=r.input.format[i];
      attrs[i] = {i, fmt ? 0u : 1u, formats[fmt], fmt ? r.input.offset[i] : i*16};
    }
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = 2;
    vertex.pVertexBindingDescriptions = bindings;
    vertex.vertexAttributeDescriptionCount = 9;
    vertex.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPrimitiveTopology topologies[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
                                        VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
                                        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                                        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
    assembly.topology = topologies[std::min(r.topology, 6u)];
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = r.cull == 1 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    raster.frontFace =
        r.cull == 2 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1;
    raster.depthBiasEnable = r.bias != 0;
    raster.depthBiasConstantFactor = -float(r.bias);
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = r.ztest != 0;
    ds.depthWriteEnable = r.zwrite != 0;
    ds.depthCompareOp = (VkCompareOp)std::clamp(int(r.zfunc) - 1, 0, 7);
    ds.stencilTestEnable = r.stencil != 0;
    VkStencilOp ops[] = {VK_STENCIL_OP_KEEP,
                         VK_STENCIL_OP_KEEP,
                         VK_STENCIL_OP_ZERO,
                         VK_STENCIL_OP_REPLACE,
                         VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                         VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                         VK_STENCIL_OP_INVERT,
                         VK_STENCIL_OP_INCREMENT_AND_WRAP,
                         VK_STENCIL_OP_DECREMENT_AND_WRAP};
    ds.front = {ops[std::min(r.fail, 8u)],
                ops[std::min(r.pass, 8u)],
                ops[std::min(r.zfail, 8u)],
                (VkCompareOp)std::clamp(int(r.func) - 1, 0, 7),
                r.readMask,
                r.writeMask,
                r.ref};
    ds.back = ds.front;
    VkBlendFactor factors[] = {VK_BLEND_FACTOR_ZERO,
                               VK_BLEND_FACTOR_ZERO,
                               VK_BLEND_FACTOR_ONE,
                               VK_BLEND_FACTOR_SRC_COLOR,
                               VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                               VK_BLEND_FACTOR_SRC_ALPHA,
                               VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                               VK_BLEND_FACTOR_DST_ALPHA,
                               VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                               VK_BLEND_FACTOR_DST_COLOR,
                               VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                               VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = r.blend != 0;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor =
        factors[std::min(r.src, 11u)];
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor =
        factors[std::min(r.dst, 11u)];
    blend.colorBlendOp = blend.alphaBlendOp =
        (VkBlendOp)std::clamp(int(r.blendop) - 1, 0, 4);
    blend.colorWriteMask = r.colorMask;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vertex;
    pi.pInputAssemblyState = &assembly;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &raster;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    pi.renderPass = pass;
    VkPipeline out;
    VK(vkCreateGraphicsPipelines(device, cache, 1, &pi, nullptr, &out));
    pipelines[r] = out;
    return out;
  }
  VkSampler sampler(const Sampler &s) {
    std::array<uint32_t, 7> key;
    memcpy(key.data(), &s, sizeof s);
    auto it = samplers.find(key);
    if (it != samplers.end())
      return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = s.mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = s.min == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = s.mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                               : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    ci.addressModeU = modes[std::min(s.u, 4u)];
    ci.addressModeV = modes[std::min(s.v, 4u)];
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.mipLodBias = std::clamp(s.bias, -properties.limits.maxSamplerLodBias,
                               properties.limits.maxSamplerLodBias);
    ci.minLod = float(s.maxMip);
    ci.maxLod = s.mip ? 1000.0f : ci.minLod;
    VkSampler out;
    VK(vkCreateSampler(device, &ci, nullptr, &out));
    samplers[key] = out;
    return out;
  }
  std::shared_ptr<Texture> upload(const TextureData &td) {
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    if (td.format == 0x31545844)
      fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    else if (td.format == 0x33545844)
      fmt = VK_FORMAT_BC2_UNORM_BLOCK;
    else if (td.format == 0x35545844)
      fmt = VK_FORMAT_BC3_UNORM_BLOCK;
    auto im =
        image(td.width, td.height, (uint32_t)td.levels.size(), fmt,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT);
    size_t total = 0;
    for (auto &l : td.levels)
      total = (total + 15) / 16 * 16 + l.size();
    auto staging = buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::vector<VkBufferImageCopy> regions;
    size_t off = 0;
    uint32_t w = td.width, h = td.height;
    for (uint32_t i = 0; i < td.levels.size(); ++i) {
      off = (off + 15) / 16 * 16;
      memcpy((char *)staging.mapped + off, td.levels[i].data(),
             td.levels[i].size());
      VkBufferImageCopy c{};
      c.bufferOffset = off;
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
      c.imageExtent = {w, h, 1};
      regions.push_back(c);
      off += td.levels[i].size();
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
    }
    VkCommandPool pool;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family;
    pc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK(vkCreateCommandPool(device, &pc, nullptr, &pool));
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(device, &ca, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cmd, &bi));
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, im->levels, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    vkCmdCopyBufferToImage(cmd, staging.handle, im->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    vkDestroyCommandPool(device, pool, nullptr);
    staging.destroy();
    return im;
  }
  ~Impl() {
    if (device) {
      vkDeviceWaitIdle(device);
      for (auto &f : frames) {
        f.retained.clear();
        f.geometry.clear();
      }
      white.reset();
      destroySwap();
      for (auto p : pipelines)
        vkDestroyPipeline(device, p.second, nullptr);
      for (auto s : samplers)
        vkDestroySampler(device, s.second, nullptr);
      for (auto v : vs)
        if (v)
          vkDestroyShaderModule(device, v, nullptr);
      if (fs)
        vkDestroyShaderModule(device, fs, nullptr);
      if (pass)
        vkDestroyRenderPass(device, pass, nullptr);
      if (cache)
        vkDestroyPipelineCache(device, cache, nullptr);
      if (layout)
        vkDestroyPipelineLayout(device, layout, nullptr);
      if (setLayout)
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      for (auto &f : frames) {
        f.upload.destroy();
        if (f.descriptors)
          vkDestroyDescriptorPool(device, f.descriptors, nullptr);
        if (f.fence)
          vkDestroyFence(device, f.fence, nullptr);
        if (f.acquired)
          vkDestroySemaphore(device, f.acquired, nullptr);
        if (f.pool)
          vkDestroyCommandPool(device, f.pool, nullptr);
      }
      vkDestroyDevice(device, nullptr);
    }
    if (surface)
      vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
};
Renderer::Renderer(Platform &platform) : p(new Impl(platform)) { p->init(); }
Renderer::~Renderer() = default;
std::shared_ptr<Texture> Renderer::upload(const TextureData &td) {
  return p->upload(td);
}
void Renderer::reset() {
  if (p->recording)
    present();
  p->dirty = true;
}
void Renderer::clear(uint32_t flags, uint32_t color, float depth,
                     uint32_t stencil) {
  if (!p->begin())
    return;
  VkClearAttachment a[2]{};
  uint32_t n = 0;
  if (flags & 1) {
    a[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    float *v = a[n].clearValue.color.float32;
    v[0] = ((color >> 16) & 255) / 255.f;
    v[1] = ((color >> 8) & 255) / 255.f;
    v[2] = (color & 255) / 255.f;
    v[3] = (color >> 24) / 255.f;
    ++n;
  }
  if (flags & 6) {
    a[n].aspectMask = ((flags & 2) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                      ((flags & 4) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    a[n].clearValue.depthStencil = {depth, stencil};
    ++n;
  }
  VkClearRect rect{{{0, 0}, p->extent}, 0, 1};
  if (n)
    vkCmdClearAttachments(p->frames[p->frameIndex].cmd, n, a, 1, &rect);
}
std::shared_ptr<Geometry> Renderer::uploadGeometry(const void *data, size_t bytes) {
  auto result = std::make_shared<GeometryBuffer>();
  result->buffer = p->buffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  memcpy(result->buffer.mapped, data, bytes);
  return result;
}
void Renderer::draw(const Raster &r, const Uniforms &u, const Vertex *v,
                    uint32_t nv, const uint32_t *indices, uint32_t ni,
                    const std::shared_ptr<Texture> *textures,
                    const Sampler *samplers, const float *viewport, const GeometryStream *geometry) {
  PerfScope timing(performance.draw);
  static const bool trace = getenv("LT2_VK_DRAW_TRACE") != nullptr;
  if(trace&&nv&&v){
    static std::set<std::string> seen;
    std::string key((const char*)&r,sizeof r);key.append((const char*)u.stage,sizeof u.stage);key.append((const char*)u.factor,16);key.append((const char*)v[0].diffuse,16);
    if(seen.size()<1000&&seen.insert(key).second){FILE*fp=fopen("vulkan-draws.log","a");if(fp){
      fprintf(fp,"draw shader=%u topology=%u vertices=%u stencil=%u/%u/%u/%u ref=%u z=%u/%u blend=%u src=%u dst=%u mask=%u lighting=%d specular=%d\n",r.shader,r.topology,nv,r.stencil,r.fail,r.pass,r.func,r.ref,r.ztest,r.zwrite,r.blend,r.src,r.dst,r.colorMask,u.flags[1],u.flags[3]);
      fprintf(fp,"factor %.3f %.3f %.3f %.3f diffuse %.3f %.3f %.3f %.3f spec %.3f %.3f %.3f %.3f\n",u.factor[0],u.factor[1],u.factor[2],u.factor[3],v[0].diffuse[0],v[0].diffuse[1],v[0].diffuse[2],v[0].diffuse[3],v[0].specular[0],v[0].specular[1],v[0].specular[2],v[0].specular[3]);
      for(int i=0;i<4;++i){fprintf(fp,"stage%d",i);for(int j=0;j<10;++j)fprintf(fp," %d",u.stage[i][j]);fprintf(fp,"\n");}fclose(fp);
    }}
  }
  if (!nv || !p->begin())
    return;
  auto &f = p->frames[p->frameIndex];
  VkBuffer vertexBuffer = f.upload.handle;
  size_t vo;
  if (geometry && geometry->resident) {
    ++performance.residentDraws;
    vertexBuffer = static_cast<GeometryBuffer*>(geometry->resident.get())->buffer.handle;
    vo = geometry->offset;
    f.geometry.push_back(geometry->resident);
  } else if (geometry) vo = p->push(geometry->data, geometry->bytes, 16);
  else vo = p->push(v, nv * sizeof(Vertex), 16);
  if(geometry)++performance.rawDraws;
  Vertex defaults{};
  defaults.pos[3]=defaults.normal[2]=defaults.weights[0]=1;
  std::fill(defaults.diffuse,defaults.diffuse+4,1.f);
  for(auto &uv:defaults.uv)uv[3]=1;
  size_t defaultOffset=p->push(&defaults,sizeof defaults,16);
  size_t io = ni ? p->push(indices, ni * 4, 4) : 0,
         uo = p->push(
             &u, sizeof u,
             (size_t)p->properties.limits.minUniformBufferOffsetAlignment);
  VkDescriptorBufferInfo buffer{f.upload.handle, 0, sizeof u};
  VkDescriptorImageInfo imgs[4];
  std::array<uint64_t, 8> key{};
  for (int i = 0; i < 4; ++i) {
    auto t = textures[i] ? textures[i] : p->white;
    imgs[i] = {p->sampler(samplers[i]), static_cast<Image *>(t.get())->view,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    key[2*i] = (uint64_t)imgs[i].sampler;
    key[2*i+1] = (uint64_t)imgs[i].imageView;
  }
  VkDescriptorSet set;
  auto found = f.textureSets.find(key);
  if (found != f.textureSets.end()) set = found->second;
  else {
  VkDescriptorSetAllocateInfo ai{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = f.descriptors;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &p->setLayout;
  VK(vkAllocateDescriptorSets(p->device, &ai, &set));
  ++performance.descriptorSets;
  for (int i = 0; i < 4; ++i) {
    auto texture = textures[i] ? textures[i] : p->white;
    // One image can appear in many sets/stages. Account for its actual memory
    // once, otherwise a stable scene exceeds the retention budget every frame.
    if (f.retainedImages.insert(texture.get()).second) {
      f.retainedBytes += static_cast<Image*>(texture.get())->allocationBytes;
      f.retained.push_back(texture);
    }
  }
  f.textureSets.emplace(key, set);
  VkWriteDescriptorSet writes[2]{};
  for (auto &w : writes) {
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
  }
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &buffer;
  writes[1].dstBinding = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[1].descriptorCount = 4;
  writes[1].pImageInfo = imgs;
  vkUpdateDescriptorSets(p->device, 2, writes, 0, nullptr);
  }
  VkViewport vp{viewport[0] * p->extent.width,
                viewport[1] * p->extent.height,
                viewport[2] * p->extent.width,
                viewport[3] * p->extent.height,
                viewport[4],
                viewport[5]};
  int left=std::clamp(int(vp.x),0,int(p->extent.width));
  int top=std::clamp(int(vp.y),0,int(p->extent.height));
  int right=std::clamp(int(vp.x+vp.width),left,int(p->extent.width));
  int bottom=std::clamp(int(vp.y+vp.height),top,int(p->extent.height));
  VkRect2D sc{{left,top},{uint32_t(right-left),uint32_t(bottom-top)}};
  vkCmdSetViewport(f.cmd, 0, 1, &vp);
  vkCmdSetScissor(f.cmd, 0, 1, &sc);
  vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline(r));
  uint32_t dynamicOffset = uint32_t(uo);
  vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0,
                          1, &set, 1, &dynamicOffset);
  VkDeviceSize offsets[] = {vo, defaultOffset};
  VkBuffer buffers[] = {vertexBuffer, f.upload.handle};
  vkCmdBindVertexBuffers(f.cmd, 0, 2, buffers, offsets);
  if (ni) {
    vkCmdBindIndexBuffer(f.cmd, f.upload.handle, io, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(f.cmd, ni, 1, 0, 0, 0);
  } else
    vkCmdDraw(f.cmd, nv, 1, 0, 0);
  ++p->draws;
}
void Renderer::present() {
  if (!p->recording)
    return;
  auto &f = p->frames[p->frameIndex];
  vkCmdEndRenderPass(f.cmd);
  Buffer capture;
  if(p->testCapture) {
    capture=p->buffer(size_t(p->extent.width)*p->extent.height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=p->images[p->imageIndex];
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(f.cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={p->extent.width,p->extent.height,1};
    vkCmdCopyImageToBuffer(f.cmd,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,capture.handle,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=0;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(f.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    host.buffer=capture.handle;host.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(f.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
  }
  VK(vkEndCommandBuffer(f.cmd));
  VK(vkResetFences(p->device, 1, &f.fence));
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &f.acquired;
  si.pWaitDstStageMask = &stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &p->complete[p->imageIndex];
  auto submitStart = PerfClock::now();
  VK(vkQueueSubmit(p->queue, 1, &si, f.fence));
  if(p->testCapture) {
    VK(vkWaitForFences(p->device,1,&f.fence,VK_TRUE,UINT64_MAX));
    FILE *file=fopen(p->testCapture,"ab");
    if(!file)throw std::runtime_error("Cannot write test capture");
    uint32_t header[]={p->extent.width,p->extent.height,uint32_t(p->format)};
    fwrite(header,sizeof header,1,file);
    fwrite(capture.mapped,size_t(p->extent.width)*p->extent.height*4,1,file);
    fclose(file);capture.destroy();
  }
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = si.pSignalSemaphores;
  pi.swapchainCount = 1;
  pi.pSwapchains = &p->swap;
  pi.pImageIndices = &p->imageIndex;
  auto result = vkQueuePresentKHR(p->queue, &pi);
  performance.present += std::chrono::duration<double, std::milli>(PerfClock::now() - submitStart).count();
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    p->dirty = true;
  else
    VK(result);
  p->recording = false;
  p->frameIndex = (p->frameIndex + 1) % 2;
  ++p->presents;
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - p->timer).count();
  if (seconds >= 5) {
    fprintf(stderr, "[vulkan] %.1f FPS, %.0f draws/frame, %zu pipelines\n",
            p->presents / seconds, double(p->draws) / p->presents,
            p->pipelines.size());
    const double frames = double(p->presents);
    fprintf(stderr, "[perf] ms/frame: adapter=%.3f draw=%.3f wait=%.3f submit=%.3f; vertices=%.0f upload=%.2f MiB descriptors=%.0f\n",
      (performance.adapter-performance.draw)/frames, performance.draw/frames,
      performance.wait/frames, performance.present/frames,
      performance.vertices/frames, performance.uploadBytes/frames/1048576.0,
      performance.descriptorSets/frames);
    fprintf(stderr,"[native render] direct=%.0f raw=%.0f resident=%.0f draws/frame\n",
      performance.directDraws/frames,performance.rawDraws/frames,performance.residentDraws/frames);
    performance = {};
    p->presents = p->draws = 0;
    p->timer = now;
  }
}
} // namespace lt2
