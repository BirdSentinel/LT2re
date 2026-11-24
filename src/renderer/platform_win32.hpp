#pragma once
#include "render.hpp"
#include <windows.h>
namespace lt2 {
class Win32Platform final : public Platform {
  HWND window;

public:
  explicit Win32Platform(HWND h) : window(h) {}
  const char *surfaceExtension() const override {
    return "VK_KHR_win32_surface";
  }
  VkSurfaceKHR surface(VkInstance instance) override;
  void extent(uint32_t &w, uint32_t &h) const override {
    RECT r{};
    GetClientRect(window, &r);
    w = r.right;
    h = r.bottom;
    if (IsIconic(window))
      w = h = 0;
  }
};
} // namespace lt2
