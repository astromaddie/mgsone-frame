#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ps1::gpu {
class GPU;
namespace vr {
class Renderer {
public:
  Renderer(GPU &gpu, const std::filesystem::path &captureDirectory);
  ~Renderer();
  bool beginFrame(uint16_t &buttons);  // true when the guest may advance
  void present();
  void rebase();
  bool running() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace vr
} // namespace ps1::gpu
