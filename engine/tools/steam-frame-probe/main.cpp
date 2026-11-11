#include "../../ps1Runtime/src/vr/xr_vulkan.h"

#include <chrono>
#include <iostream>

namespace {
using ps1::gpu::vr::FrameRuntime;
using ps1::gpu::vr::vkCheck;

void clearEyes(FrameRuntime &xr) {
  std::array<uint32_t, 2> indices{};
  for (size_t i = 0; i < indices.size(); ++i) indices[i] = xr.acquire(i);
  vkCheck(vkResetCommandBuffer(xr.commands, 0), "Reset probe commands");
  const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkCheck(vkBeginCommandBuffer(xr.commands, &begin), "Begin probe commands");
  for (size_t i = 0; i < indices.size(); ++i) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = xr.eyes[i].images.at(indices[i]).image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(xr.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkClearColorValue color{{i == 0 ? 1.0f : 0.0f, 0, i == 1 ? 1.0f : 0.0f, 1.0f}};
    vkCmdClearColorImage(xr.commands, barrier.image, barrier.newLayout, &color, 1, &barrier.subresourceRange);
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(xr.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
  vkCheck(vkEndCommandBuffer(xr.commands), "End probe commands");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &xr.commands;
  vkCheck(vkQueueSubmit(xr.queue, 1, &submit, VK_NULL_HANDLE), "Submit probe eyes");
  vkCheck(vkQueueWaitIdle(xr.queue), "Complete probe clears");
  for (size_t i = 0; i < indices.size(); ++i) xr.release(i);
}
} // namespace

int main() {
  using namespace std::chrono_literals;
  using ps1::gpu::vr::EyeView;
  using ps1::gpu::vr::xrCheck;
  std::cout << std::unitbuf;
  try {
    FrameRuntime xr;
    xr.open("MGS Frame Probe");
    xr.swapchains(512);
    std::array<EyeView, 2> poses{};
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    unsigned submitted = 0;
    bool exitRequested = false;
    while (!xr.finished && std::chrono::steady_clock::now() < deadline) {
      const bool draw = xr.wait(poses);
      if (draw) {
        clearEyes(xr);
        ++submitted;
      }
      xr.end(draw);
      if (!exitRequested && xr.active &&
          (submitted >= 90 || std::chrono::steady_clock::now() >= deadline - 2s)) {
        xrCheck(xrRequestExitSession(xr.session), "xrRequestExitSession");
        exitRequested = true;
      }
      if (exitRequested && !xr.active) break;
    }
    std::cout << "Submitted " << submitted << " red-left / blue-right projection frames.\n";
    return submitted ? 0 : 2;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
