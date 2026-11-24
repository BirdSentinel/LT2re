#define VK_USE_PLATFORM_WIN32_KHR
#include "platform_win32.hpp"
#include <stdexcept>
VkSurfaceKHR lt2::Win32Platform::surface(VkInstance instance) {
  VkWin32SurfaceCreateInfoKHR ci{
      VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  ci.hinstance = GetModuleHandleW(nullptr);
  ci.hwnd = window;
  VkSurfaceKHR out{};
  auto create = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(
      instance, "vkCreateWin32SurfaceKHR");
  if (!create || create(instance, &ci, nullptr, &out) != VK_SUCCESS)
    throw std::runtime_error("Win32 Vulkan surface creation failed");
  return out;
}
