#include "mgs_vr.h"

#include "xr_vulkan.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_vr_input.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/gpu/display_geometry.h"
#include "runtime/gpu/gpu.h"
#include "runtime/gpu/mgs_asset_cache_vulkan.h"
#include "runtime/input/input.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numbers>
#include <span>
#include <vector>

#ifndef PS1RECOMP_VULKAN_SHADER_DIR
#error The VR renderer requires the compiled Vulkan shaders
#endif

namespace ps1::gpu::vr {
namespace {
constexpr uint32_t maxFaces = 32768;
constexpr uint32_t captureSize = 768;
constexpr uint32_t vramWords = GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT;
constexpr float gameUnitsPerMetre = 1000;

struct Vertex {
  Vec3 position;
  std::array<float, 2> uv;
  Vec3 shade;
  uint32_t material;
};

struct alignas(16) Material {
  std::array<uint32_t, 4> data{};
  std::array<int32_t, 4> origin{};
  std::array<uint32_t, 4> native{};
  std::array<uint32_t, 8> palette{};
};
static_assert(sizeof(Material) == 80);
static_assert(sizeof(Color16) == 2);

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  VkDeviceSize size = 0;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

struct EyeTarget {
  VkExtent2D extent{};
  Image color, depth;
  std::vector<VkImageView> swapchainViews;
  std::vector<VkFramebuffer> framebuffers;
};

float dot(const Vec3 &a, const Vec3 &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3 &a, const Vec3 &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

Vec3 unit(Vec3 v) {
  const float length = std::sqrt(dot(v, v));
  if (length > 1e-8f)
    for (auto &x : v) x /= length;
  return v;
}

Vec3 anchorSpace(const Vec3 &eye) {
  const float widescreen = games::mgs::widescreen16x9() ? 0.75f : 1;
  return {eye[0] / (gameUnitsPerMetre * widescreen),
          -eye[1] / (gameUnitsPerMetre * (58.0f / 64.0f)),
          -eye[2] / gameUnitsPerMetre};
}

struct PlayerCamera {
  Vec3 origin, right, up, forward;

  PlayerCamera(const replay::MgsPlayerView &player, float heading) {
    const auto &view = player.sourceView;
    const auto direction = [&](const Vec3 &v) {
      return Vec3{view[0] * v[0] + view[4] * v[1] + view[8] * v[2],
                  view[1] * v[0] + view[5] * v[1] + view[9] * v[2],
                  view[2] * v[0] + view[6] * v[1] + view[10] * v[2]};
    };
    Vec3 eye = direction(player.eye);
    for (unsigned axis = 0; axis < 3; ++axis) eye[axis] += view[12 + axis];
    origin = anchorSpace(eye);
    forward = unit(anchorSpace(direction({std::sin(heading), 0, std::cos(heading)})));
    right = unit(cross(forward, anchorSpace(direction({0, 1, 0}))));
    up = cross(right, forward);
  }

  Vec3 transform(Vec3 point) const {
    for (unsigned axis = 0; axis < 3; ++axis) point[axis] -= origin[axis];
    return {dot(point, right), dot(point, up), -dot(point, forward)};
  }
};

Vec3 shadeVertex(const replay::DrawPacket &packet, unsigned lane) {
  const auto &v = packet.vertices[lane];
  Vec3 color{float(v.color.r), float(v.color.g), float(v.color.b)};
  if (packet.rawTexture && packet.textured)
    color = {128, 128, 128};
  else if (packet.originalMesh && packet.originalMesh->lighting.ambient[3] != 0) {
    const auto &light = packet.originalMesh->lighting;
    const auto &n = light.normals[lane];
    Vec3 normal{n[0], n[1], n[2]};
    const float length = std::sqrt(dot(normal, normal));
    if (length > 1e-6f)
      for (auto &x : normal) x /= length;
    Vec3 incidence{};
    for (unsigned i = 0; i < 3; ++i) {
      const auto &d = light.directions[i];
      incidence[i] = std::max(0.0f, dot(normal, {d[0], d[1], d[2]}));
    }
    for (unsigned i = 0; i < 3; ++i)
      color[i] = light.ambient[i] +
          dot(incidence, {light.colors[i][0], light.colors[i][1], light.colors[i][2]});
    const auto &position = light.positions[lane];
    for (const auto &point : light.pointLights) {
      const auto &p = point.positionRadius;
      const auto &e = point.colorExtent;
      const Vec3 delta{position[0] - p[0], position[1] - p[1], position[2] - p[2]};
      const float distance = std::sqrt(dot(delta, delta));
      const float extent = std::max({std::abs(delta[0]), std::abs(delta[1]), std::abs(delta[2])});
      const float t = std::clamp((extent - e[3] * 0.9f) / std::max(0.001f, e[3] * 0.1f), 0.0f, 1.0f);
      const float gate = 1 - t * t * (3 - 2 * t);
      const float strength = std::max(0.0f, dot(normal, delta) / std::max(0.001f, distance)) *
          std::max(0.0f, 0.75f * (2 - distance / std::max(0.001f, p[3]))) * gate;
      for (unsigned i = 0; i < 3; ++i) color[i] += e[i] * strength;
    }
  }
  for (auto &x : color) x = std::max(0.0f, x) / (packet.textured ? 128.0f : 255.0f);
  return color;
}

bool meshPosition(std::span<const uint8_t> bytes,
                  const replay::OriginalMeshGeometry &mesh,
                  unsigned lane, Vec3 &out) {
  Vec3 eye{};
  for (unsigned corner = 0; corner < 4; ++corner) {
    const float weight = mesh.weights[lane][corner];
    if (weight == 0) continue;
    const uint32_t reference = mesh.pointBytes[corner];
    const bool packed = reference >> 31;
    const size_t offset = reference & 0x7fffffffu;
    if (offset > bytes.size() || bytes.size() - offset < (packed ? 6u : 12u))
      return false;
    Vec3 point{};
    for (unsigned axis = 0; axis < 3; ++axis) {
      if (packed) {
        int16_t value;
        std::memcpy(&value, bytes.data() + offset + axis * 2, 2);
        point[axis] = value;
      } else {
        std::memcpy(&point[axis], bytes.data() + offset + axis * 4, 4);
      }
    }
    for (unsigned row = 0; row < 3; ++row) {
      const auto &r = mesh.viewRows[corner * 3 + row];
      eye[row] += weight * (dot(point, {r[0], r[1], r[2]}) + r[3]);
    }
  }
  out = anchorSpace(eye);
  return std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); });
}

Material materialFor(const replay::DrawPacket &packet) {
  Material result;
  if (packet.originalTexture) {
    result.data = packet.originalTexture->data;
    result.origin = packet.originalTexture->origin;
    result.palette = packet.originalTexture->palette;
  }
  uint32_t opacityAndFlags = packet.state.blend.enabled ? 128u : 255u;
  if (packet.originalMesh && packet.originalMesh->stereoCullBack)
    opacityAndFlags |= 1u << 8;
  result.native = {packet.state.texturePage.raw, packet.state.clut.raw,
                   packet.textured ? 1u : 0u, opacityAndFlags};
  return result;
}

std::filesystem::path shaderPath(const char *name) {
  const char *overridePath = std::getenv("PS1RECOMP_VULKAN_SHADER_DIR");
  return std::filesystem::path(overridePath && *overridePath ? overridePath :
                              PS1RECOMP_VULKAN_SHADER_DIR) / name;
}

std::array<EyeView, 2> diagnosticEyes(float angle = 0, float lean = 0) {
  std::array<EyeView, 2> eyes{};
  for (size_t i = 0; i < eyes.size(); ++i) {
    eyes[i].orientation = yaw(angle);
    eyes[i].position = rotate(eyes[i].orientation, {i ? 0.032f : -0.032f, 0, 0});
    eyes[i].position[0] += lean;
  }
  return eyes;
}

struct PadBinding {
  input::Button bit;
  const char *name;
  const char *label;
  const char *path;
};
constexpr PadBinding padBindings[]{
    {input::BTN_CROSS, "cross", "Cross", "/user/hand/right/input/a/click"},
    {input::BTN_CIRCLE, "circle", "Circle", "/user/hand/right/input/b/click"},
    {input::BTN_SQUARE, "square", "Square", "/user/hand/right/input/x/click"},
    {input::BTN_TRIANGLE, "triangle", "Triangle", "/user/hand/right/input/y/click"},
    {input::BTN_START, "start", "Start", "/user/hand/right/input/menu/click"},
    {input::BTN_SELECT, "select", "Select", "/user/hand/left/input/view/click"},
    {input::BTN_UP, "up", "Up", "/user/hand/left/input/dpad_up/click"},
    {input::BTN_DOWN, "down", "Down", "/user/hand/left/input/dpad_down/click"},
    {input::BTN_LEFT, "left", "Left", "/user/hand/left/input/dpad_left/click"},
    {input::BTN_RIGHT, "right", "Right", "/user/hand/left/input/dpad_right/click"},
    {input::BTN_L1, "l1", "L1", "/user/hand/left/input/bumper/click"},
    {input::BTN_R1, "r1", "R1", "/user/hand/right/input/bumper/click"},
    {input::BTN_L2, "l2", "L2", "/user/hand/left/input/trigger/click"},
    {input::BTN_R2, "r2", "R2", "/user/hand/right/input/trigger/click"},
    {input::BTN_L3, "l3", "L3", "/user/hand/left/input/thumbstick/click"},
    {input::BTN_R3, "r3", "R3", "/user/hand/right/input/thumbstick/click"},
};

struct FramePad {
  XrActionSet set = XR_NULL_HANDLE;
  std::array<XrAction, std::size(padBindings)> buttons{};
  XrAction stick = XR_NULL_HANDLE;
  XrAction turnStick = XR_NULL_HANDLE, aim = XR_NULL_HANDLE;
  XrAction weaponMenu = XR_NULL_HANDLE;
  XrSpace aimSpace = XR_NULL_HANDLE;
  XrVector2f move{};
  float turn = 0;
  float grip = 0;
  XrPosef aimPose{};
  bool aimValid = false, sticksReady = false, gripReady = false;
  uint16_t heldThroughFocus = 0xffff;
  bool attached = false;

  ~FramePad() {
    if (aimSpace) xrDestroySpace(aimSpace);
    if (set) xrDestroyActionSet(set);
  }

  void attach(FrameRuntime &xr) {
    if (!xr.frameController) {
      std::cerr << "[MGS VR] Steam Frame controller profile unavailable; using SDL input\n";
      return;
    }
    XrActionSetCreateInfo create{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(create.actionSetName, "mgs_pad");
    std::strcpy(create.localizedActionSetName, "MGS Controller");
    xrCheck(xrCreateActionSet(xr.xr, &create, &set), "Create MGS action set");
    const auto path = [&](const char *name) {
      XrPath value = XR_NULL_PATH;
      xrCheck(xrStringToPath(xr.xr, name, &value), "Read Frame controller path");
      return value;
    };
    const auto action = [&](XrActionType type, const char *name, const char *label) {
      XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
      info.actionType = type;
      std::strcpy(info.actionName, name);
      std::strcpy(info.localizedActionName, label);
      XrAction value = XR_NULL_HANDLE;
      xrCheck(xrCreateAction(set, &info, &value), "Create MGS pad action");
      return value;
    };
    std::array<XrActionSuggestedBinding, std::size(padBindings) + 4> bindings{};
    for (size_t i = 0; i < buttons.size(); ++i) {
      const auto &item = padBindings[i];
      buttons[i] = action(XR_ACTION_TYPE_BOOLEAN_INPUT, item.name, item.label);
      bindings[i] = {buttons[i], path(item.path)};
    }
    stick = action(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move");
    turnStick = action(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Turn");
    aim = action(XR_ACTION_TYPE_POSE_INPUT, "aim", "Aim");
    weaponMenu = action(XR_ACTION_TYPE_FLOAT_INPUT, "weapon_menu", "Weapon Menu");
    bindings[buttons.size()] = {stick, path("/user/hand/left/input/thumbstick")};
    bindings[buttons.size() + 1] = {turnStick, path("/user/hand/right/input/thumbstick")};
    bindings[buttons.size() + 2] = {aim, path("/user/hand/right/input/aim/pose")};
    bindings[buttons.size() + 3] = {weaponMenu, path("/user/hand/right/input/squeeze/value")};
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestion.interactionProfile = path("/interaction_profiles/valve/frame_controller_valve");
    suggestion.countSuggestedBindings = bindings.size();
    suggestion.suggestedBindings = bindings.data();
    const XrResult result = xrSuggestInteractionProfileBindings(xr.xr, &suggestion);
    std::cout << "[MGS VR] Steam Frame controller bindings: " << result << '\n';
    xrCheck(result, "Suggest Steam Frame controller bindings");
    const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
    xrCheck(xrAttachSessionActionSets(xr.session, &attach), "Attach MGS controller actions");
    XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space.action = aim;
    space.poseInActionSpace.orientation.w = 1;
    xrCheck(xrCreateActionSpace(xr.session, &space, &aimSpace), "Create right controller aim space");
    attached = true;
  }

  uint16_t sample(const FrameRuntime &xr, bool focused) {
    move = {};
    turn = 0;
    grip = 0;
    aimValid = false;
    if (!attached || !focused) {
      heldThroughFocus = 0xffff;
      sticksReady = false;
      gripReady = false;
      return 0;
    }
    const XrActiveActionSet active{set, XR_NULL_PATH};
    const XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    const XrResult result = xrSyncActions(xr.session, &sync);
    xrCheck(result, "Sync MGS controller actions");
    if (result == XR_SESSION_NOT_FOCUSED) {
      heldThroughFocus = 0xffff;
      sticksReady = gripReady = false;
      return 0;
    }
    uint16_t pressed = 0;
    for (size_t i = 0; i < buttons.size(); ++i) {
      const XrActionStateGetInfo query{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, buttons[i], XR_NULL_PATH};
      XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
      xrCheck(xrGetActionStateBoolean(xr.session, &query, &state), "Read MGS pad button");
      if (state.isActive && state.currentState) pressed |= padBindings[i].bit;
    }
    const XrActionStateGetInfo query{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, stick, XR_NULL_PATH};
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrCheck(xrGetActionStateVector2f(xr.session, &query, &state), "Read MGS movement stick");
    const XrActionStateGetInfo turnQuery{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, turnStick, XR_NULL_PATH};
    XrActionStateVector2f turnState{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrCheck(xrGetActionStateVector2f(xr.session, &turnQuery, &turnState), "Read MGS turning stick");
    const XrActionStateGetInfo gripQuery{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, weaponMenu, XR_NULL_PATH};
    XrActionStateFloat gripState{XR_TYPE_ACTION_STATE_FLOAT};
    xrCheck(xrGetActionStateFloat(xr.session, &gripQuery, &gripState), "Read MGS weapon menu grip");
    if (gripState.isActive) grip = gripState.currentState;
    if (grip < 0.25f) gripReady = true;
    if (!gripReady) grip = 0;
    if (std::hypot(state.currentState.x, state.currentState.y) < 0.25f &&
        std::abs(turnState.currentState.x) < 0.25f)
      sticksReady = true;
    if (sticksReady) {
      if (state.isActive) move = state.currentState;
      if (turnState.isActive && std::abs(turnState.currentState.x) > 0.25f)
        turn = turnState.currentState.x;
    }
    const XrActionStateGetInfo aimQuery{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, aim, XR_NULL_PATH};
    XrActionStatePose aimState{XR_TYPE_ACTION_STATE_POSE};
    xrCheck(xrGetActionStatePose(xr.session, &aimQuery, &aimState), "Read right controller tracking");
    if (aimState.isActive) {
      XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
      xrCheck(xrLocateSpace(aimSpace, xr.space, xr.frameState.predictedDisplayTime, &location),
              "Locate right controller aim");
      constexpr auto valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
      aimValid = (location.locationFlags & valid) == valid;
      if (aimValid) aimPose = location.pose;
    }
    if (state.isActive && sticksReady) {
      if (state.currentState.x < -0.37f) pressed |= input::BTN_LEFT;
      if (state.currentState.x > 0.37f) pressed |= input::BTN_RIGHT;
      if (state.currentState.y < -0.37f) pressed |= input::BTN_DOWN;
      if (state.currentState.y > 0.37f) pressed |= input::BTN_UP;
    }
    heldThroughFocus &= pressed;
    return pressed & ~heldThroughFocus;
  }
};
} // namespace

struct Renderer::Impl {
  explicit Impl(GPU &source, std::filesystem::path output)
      : gpu(source), directory(std::move(output)), vram(vramWords) {}

  GPU &gpu;
  std::filesystem::path directory;
  FrameRuntime xr;
  FramePad pad;
  std::unique_ptr<MgsAssetGpuCache> assets;
  std::array<EyeTarget, 2> targets;
  VkRenderPass pass = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline worldPipeline = VK_NULL_HANDLE, translucentPipeline = VK_NULL_HANDLE,
             panelPipeline = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  Buffer nativeBuffer, vertexBuffer, materialBuffer, readback;
  VkShaderModule vertexShader = VK_NULL_HANDLE, fragmentShader = VK_NULL_HANDLE;
  std::vector<Color16> vram;
  std::vector<Vertex> vertices;
  std::vector<Material> materials;
  std::vector<const replay::DrawPacket *> transparent;
  std::array<EyeView, 2> eyes{};
  Anchor anchor{};
  GPU::DisplayState display{};
  replay::CrashReplayFramePtr worldFrame;
  std::optional<PlayerCamera> playerCamera;
  uint64_t revision = UINT64_MAX, builtFrame = UINT64_MAX;
  uint64_t lastWorld = 0, captureAt = 0, submitted = 0;
  uint32_t solidVertices = 0, worldVertices = 0, worldFaces = 0;
  bool canDraw = false, captured = false, cacheBound = false;
  bool firstPerson = false, switchHeld = false;
  uint64_t playerEpoch = 0, playerLifetime = 0;
  float baseHeading = 0, stickTurn = 0, headTurn = 0;

  bool offscreen() const { return !directory.empty(); }
  VkDevice device() const { return xr.device; }

  ~Impl() {
    games::mgs::setVrShotDirection({});
    if (!device()) return;
    vkDeviceWaitIdle(device());
    if (cacheBound) {
      games::mgs::setSceneAssetCache(nullptr);
      games::mgs::setMeshReplayStereo(false);
    }
    assets.reset();
    for (auto &eye : targets) {
      for (auto f : eye.framebuffers) vkDestroyFramebuffer(device(), f, nullptr);
      for (auto view : eye.swapchainViews) vkDestroyImageView(device(), view, nullptr);
      destroyImage(eye.color);
      destroyImage(eye.depth);
    }
    vkDestroyFence(device(), fence, nullptr);
    vkDestroyPipeline(device(), panelPipeline, nullptr);
    vkDestroyPipeline(device(), translucentPipeline, nullptr);
    vkDestroyPipeline(device(), worldPipeline, nullptr);
    vkDestroyPipelineLayout(device(), pipelineLayout, nullptr);
    vkDestroyShaderModule(device(), fragmentShader, nullptr);
    vkDestroyShaderModule(device(), vertexShader, nullptr);
    vkDestroyDescriptorPool(device(), pool, nullptr);
    vkDestroyDescriptorSetLayout(device(), setLayout, nullptr);
    vkDestroyRenderPass(device(), pass, nullptr);
    for (auto *b : {&nativeBuffer, &vertexBuffer, &materialBuffer, &readback}) {
      if (b->mapped) vkUnmapMemory(device(), b->memory);
      vkDestroyBuffer(device(), b->buffer, nullptr);
      vkFreeMemory(device(), b->memory, nullptr);
    }
  }

  void destroyImage(Image &image) {
    vkDestroyImageView(device(), image.view, nullptr);
    vkDestroyImage(device(), image.image, nullptr);
    vkFreeMemory(device(), image.memory, nullptr);
  }

  uint32_t memoryType(uint32_t allowed, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(xr.physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((allowed & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No matching memory type for the MGS VR renderer");
  }

  void createBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
    buffer.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    vkCheck(vkCreateBuffer(device(), &info, nullptr, &buffer.buffer), "Create VR buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device(), buffer.buffer, &requirements);
    const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr, requirements.size, memoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkCheck(vkAllocateMemory(device(), &allocation, nullptr, &buffer.memory), "Allocate VR buffer");
    vkCheck(vkBindBufferMemory(device(), buffer.buffer, buffer.memory, 0), "Bind VR buffer");
    vkCheck(vkMapMemory(device(), buffer.memory, 0, size, 0, &buffer.mapped), "Map VR buffer");
  }

  VkImageView imageView(VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.format = format;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    vkCheck(vkCreateImageView(device(), &info, nullptr, &view), "Create VR image view");
    return view;
  }

  void createImage(Image &image, VkExtent2D extent, VkFormat format,
                   VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = {extent.width, extent.height, 1};
    info.format = format;
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    vkCheck(vkCreateImage(device(), &info, nullptr, &image.image), "Create VR image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device(), image.image, &requirements);
    const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr, requirements.size, memoryType(requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkCheck(vkAllocateMemory(device(), &allocation, nullptr, &image.memory), "Allocate VR image");
    vkCheck(vkBindImageMemory(device(), image.image, image.memory, 0), "Bind VR image");
    image.view = imageView(image.image, format, aspect);
  }

  void initTargets() {
    constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    const auto finalLayout = offscreen() ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    std::array<VkAttachmentDescription, 2> attachments{{
        {0, xr.colorFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
         VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, finalLayout},
        {0, depthFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}
    }};
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    VkSubpassDependency copy{0, VK_SUBPASS_EXTERNAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = attachments.size();
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    if (offscreen()) {
      info.dependencyCount = 1;
      info.pDependencies = &copy;
    }
    vkCheck(vkCreateRenderPass(device(), &info, nullptr, &pass), "Create VR render pass");

    for (size_t i = 0; i < targets.size(); ++i) {
      auto &eye = targets[i];
      std::vector<VkImageView> views;
      if (offscreen()) {
        eye.extent = {captureSize, captureSize};
        createImage(eye.color, eye.extent, xr.colorFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
        views.push_back(eye.color.view);
      } else {
        const auto &source = xr.eyes[i];
        eye.extent = {uint32_t(source.extent.width), uint32_t(source.extent.height)};
        for (const auto &image : source.images)
          eye.swapchainViews.push_back(imageView(image.image, xr.colorFormat, VK_IMAGE_ASPECT_COLOR_BIT));
        views = eye.swapchainViews;
      }
      createImage(eye.depth, eye.extent, depthFormat,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
      for (auto view : views) {
        const VkImageView attachmentsForEye[]{view, eye.depth.view};
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = pass;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = attachmentsForEye;
        framebuffer.width = eye.extent.width;
        framebuffer.height = eye.extent.height;
        framebuffer.layers = 1;
        VkFramebuffer result = VK_NULL_HANDLE;
        vkCheck(vkCreateFramebuffer(device(), &framebuffer, nullptr, &result), "Create VR eye framebuffer");
        eye.framebuffers.push_back(result);
      }
    }
  }

  VkShaderModule loadShader(const char *name) {
    const auto path = shaderPath(name);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = file.tellg();
    if (!file || size <= 0 || size % 4)
      throw std::runtime_error("Invalid VR shader " + path.string());
    std::vector<uint32_t> code(size_t(size) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(code.data()), size))
      throw std::runtime_error("Cannot read VR shader " + path.string());
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size_t(size);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device(), &info, nullptr, &module), "Create VR shader");
    return module;
  }

  VkPipeline makePipeline(bool depthTest, bool depthWrite, bool blend) {
    const VkPipelineShaderStageCreateInfo stages[]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertexShader, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "main", nullptr}
    };
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, shade)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, material)}
    };
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 4;
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo sample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    sample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depthTest;
    depth.depthWriteEnable = depthWrite;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    attachment.blendEnable = blend;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkPipelineColorBlendStateCreateInfo color{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color.attachmentCount = 1;
    color.pAttachments = &attachment;
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &sample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &color;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout;
    info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCheck(vkCreateGraphicsPipelines(device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "Create VR graphics pipeline");
    return pipeline;
  }

  void init() {
    if (offscreen()) {
      std::filesystem::create_directories(directory);
      xr.openOffscreen();
      if (const char *value = std::getenv("PS1_VR_CAPTURE_VBLANK"))
        captureAt = std::stoull(value);
      if (const char *value = std::getenv("PS1_VR_FIRST_PERSON"))
        firstPerson = std::strcmp(value, "1") == 0;
      eyes = diagnosticEyes();
    } else {
      xr.open("Midnight Ghost Stone", true);
      pad.attach(xr);
      xr.swapchains(1728);
    }
    const char *cache = std::getenv("PS1_MGS_ASSET_CACHE");
    if (!cache || !*cache) throw std::runtime_error("MGS VR requires an imported asset cache");
    assets = MgsAssetGpuCache::upload(xr.physical, device(), xr.queue, xr.pool,
        games::mgs::AssetCache::load(cache), false);
    initTargets();
    createBuffer(nativeBuffer, vramWords * sizeof(Color16), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    createBuffer(vertexBuffer, maxFaces * 6 * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    createBuffer(materialBuffer, maxFaces * sizeof(Material), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (offscreen())
      createBuffer(readback, captureSize * captureSize * 4u * 2u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
      bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 nullptr, 0, uint32_t(bindings.size()), bindings.data()};
    vkCheck(vkCreateDescriptorSetLayout(device(), &setInfo, nullptr, &setLayout), "Create VR descriptors");
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    const VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             nullptr, 0, 1, 1, &poolSize};
    vkCheck(vkCreateDescriptorPool(device(), &poolInfo, nullptr, &pool), "Create VR descriptor pool");
    const VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               nullptr, pool, 1, &setLayout};
    vkCheck(vkAllocateDescriptorSets(device(), &allocate, &descriptor), "Allocate VR descriptor set");
    const VkDescriptorBufferInfo buffers[]{
        {assets->buffer(), 0, assets->assets().bytes().size()},
        {nativeBuffer.buffer, 0, nativeBuffer.size},
        {materialBuffer.buffer, 0, materialBuffer.size}
    };
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor, i, 0, 1,
                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[i], nullptr};
    }
    vkUpdateDescriptorSets(device(), writes.size(), writes.data(), 0, nullptr);
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Matrix)};
    const VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                           nullptr, 0, 1, &setLayout, 1, &push};
    vkCheck(vkCreatePipelineLayout(device(), &layout, nullptr, &pipelineLayout), "Create VR pipeline layout");
    vertexShader = loadShader("mgs_scene.vert.spv");
    fragmentShader = loadShader("mgs_scene.frag.spv");
    worldPipeline = makePipeline(true, true, false);
    translucentPipeline = makePipeline(true, false, true);
    panelPipeline = makePipeline(false, false, true);
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCheck(vkCreateFence(device(), &fenceInfo, nullptr, &fence), "Create VR completion fence");

    games::mgs::setMeshReplayStereo(true);
    games::mgs::setSceneAssetCache(&assets->assets());
    cacheBound = true;
    gpu.configureReplay({true, 4, true, true, replay::ReplayDepthMode::Hybrid});
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(xr.physical, &properties);
    std::cout << "[MGS VR] " << (offscreen() ? "Offscreen stereo" : "OpenXR stereo")
              << " on " << properties.deviceName << '\n';
  }

  bool addPacket(const replay::DrawPacket &packet, const DisplayAperture &aperture, bool world) {
    if (materials.size() >= maxFaces - 2 || packet.vertexCount > 4 ||
        packet.indexCount == 0 || packet.indexCount > 6)
      return false;
    const uint32_t material = materials.size();
    std::array<Vertex, 4> points{};
    for (unsigned i = 0; i < packet.vertexCount; ++i) {
      auto &v = points[i];
      const auto &p = packet.vertices[i];
      if (world) {
        if (!meshPosition(assets->assets().bytes(), *packet.originalMesh, i, v.position))
          return false;
        if (playerCamera) v.position = playerCamera->transform(v.position);
      } else {
        constexpr float panelDepth = 2.2f;
        v.position = {float((p.x - aperture.x) / aperture.width - 0.5) * 1.9f,
                      float(0.5 - (p.y - aperture.y) / aperture.height) * 1.06875f,
                      -panelDepth};
      }
      v.uv = {float(p.u), float(p.v)};
      v.shade = shadeVertex(packet, i);
      v.material = material;
    }
    for (unsigned i = 0; i < packet.indexCount; ++i)
      if (packet.triangleIndices[i] >= packet.vertexCount) return false;
    materials.push_back(materialFor(packet));
    for (unsigned i = 0; i < packet.indexCount; ++i)
      vertices.push_back(points[packet.triangleIndices[i]]);
    // The PS1 quad strip flips the second triangle's winding.
    if (world && packet.vertexCount == 4 && packet.indexCount == 6)
      std::iter_swap(vertices.end() - 2, vertices.end() - 1);
    return true;
  }

  void cinema() {
    const auto scanout = deriveDisplayScanoutPlan(rawDisplayCrtcInput(display),
                                                 DisplayAreaPolicy::Programmed);
    if (!scanout.enabled) return;
    const bool packed = scanout.format == DisplayScanoutFormat::PackedRGB888;
    Material material{};
    material.native = {0, 0, packed ? 3u : 2u, 255};
    material.data = {scanout.sourceByteX, scanout.sourceY, scanout.rowStep, 0};
    const float x = packed ? 0 : float(scanout.sourceByteX / 2);
    const float width = float(scanout.activeExtent.width);
    const float height = float(scanout.activeExtent.height);
    materials.push_back(material);
    constexpr float halfWidth = 1.3f;
    constexpr float halfHeight = halfWidth * 9 / 16;
    constexpr float depth = -3;
    const Vec3 white{1, 1, 1};
    const Vertex a{{-halfWidth, halfHeight, depth}, {x, 0}, white, 0};
    const Vertex b{{halfWidth, halfHeight, depth}, {x + width, 0}, white, 0};
    const Vertex c{{-halfWidth, -halfHeight, depth}, {x, height}, white, 0};
    const Vertex d{{halfWidth, -halfHeight, depth}, {x + width, height}, white, 0};
    vertices.insert(vertices.end(), {a, b, c, b, d, c});
  }

  void updateScene() {
    replay::CrashReplayFramePtr frame;
    const auto status = gpu.copyDisplayVRAMIfChanged(vram, revision, &display, &frame);
    while (gpu.tryConsumeReplay()) {}
    if (status == GPU::DisplayCopyStatus::Copied) {
      revision = display.contentRevision;
      std::memcpy(nativeBuffer.mapped, vram.data(), nativeBuffer.size);
    }
    if (builtFrame == display.latchFrame) return;
    builtFrame = display.latchFrame;
    const auto isWorld = [](const replay::DrawPacket &p) {
      return p.originalMesh && p.originalMesh->sourceTopology &&
             p.route == replay::PacketRoute::Projected3D;
    };
    bool haveWorld = false, codec = false;
    if (frame) for (const auto &op : frame->operations()) {
      if (const auto *packet = std::get_if<replay::DrawPacket>(&op)) {
        haveWorld |= isWorld(*packet);
        codec |= packet->screenEffect.kind == replay::MgsScreenEffectKind::CodecScreen;
      }
    }
    const auto aperture = deriveDisplayAperture(display, DisplayAreaPolicy::Programmed);
    if (codec || !aperture.enabled || builtFrame < lastWorld || builtFrame - lastWorld > 6)
      worldFrame.reset();
    if (haveWorld && !codec) {
      worldFrame = frame;
      lastWorld = builtFrame;
    }

    vertices.clear();
    materials.clear();
    transparent.clear();
    playerCamera.reset();
    worldFaces = solidVertices = worldVertices = 0;
    if (worldFrame && aperture.enabled) {
      const auto &player = worldFrame->latch().mgsPlayerView;
      if (firstPerson && player) {
        if (playerEpoch != player->sceneEpoch || playerLifetime != player->bodyLifetime) {
          playerEpoch = player->sceneEpoch;
          playerLifetime = player->bodyLifetime;
          baseHeading = player->yaw * (2 * std::numbers::pi_v<float> / 4096);
          stickTurn = 0;
          headTurn = 0;
        }
        playerCamera.emplace(*player, baseHeading);
      }
      for (const auto &op : worldFrame->operations()) {
        if (const auto *packet = std::get_if<replay::DrawPacket>(&op); packet && isWorld(*packet)) {
          const auto &identity = packet->originalMesh->temporalIdentity;
          if (playerCamera && identity[0] == player->sceneEpoch && identity[1] == player->bodyLifetime)
            continue;
          if (packet->state.blend.enabled) transparent.push_back(packet);
          else if (addPacket(*packet, aperture, true)) ++worldFaces;
        }
      }
      solidVertices = vertices.size();
      for (auto *packet : transparent)
        if (addPacket(*packet, aperture, true)) ++worldFaces;
      worldVertices = vertices.size();
    }
    if (worldFaces == 0) {
      vertices.clear();
      materials.clear();
      cinema();
    } else if (frame) {
      for (const auto &op : frame->operations()) {
        const auto *packet = std::get_if<replay::DrawPacket>(&op);
        if (!packet || packet->route != replay::PacketRoute::Screen2D || packet->originalMesh ||
            packet->screenEffect.kind != replay::MgsScreenEffectKind::None)
          continue;
        bool inside = packet->vertexCount > 0 && packet->vertexCount <= 4;
        for (unsigned i = 0; inside && i < packet->vertexCount; ++i) {
          const auto &v = packet->vertices[i];
          inside = v.x >= aperture.x && v.x <= aperture.x + aperture.width &&
                   v.y >= aperture.y && v.y <= aperture.y + aperture.height;
        }
        if (inside) addPacket(*packet, aperture, false);
      }
    }
    if (!vertices.empty())
      std::memcpy(vertexBuffer.mapped, vertices.data(), vertices.size() * sizeof(Vertex));
    if (!materials.empty())
      std::memcpy(materialBuffer.mapped, materials.data(), materials.size() * sizeof(Material));
  }

  void drawEyes(const std::array<EyeView, 2> &views) {
    const bool showAim = playerCamera && pad.aimValid;
    std::array<Vertex, 12> reticle{};
    if (showAim) {
      const auto &q = pad.aimPose.orientation;
      const auto &p = pad.aimPose.position;
      const Vec3 origin{p.x, p.y, p.z};
      const auto direction = rotate({q.x, q.y, q.z, q.w}, {0, 0, -1});
      Vec3 target{};
      for (unsigned axis = 0; axis < 3; ++axis)
        target[axis] = origin[axis] + direction[axis] * 3.5f;
      const auto right = rotate(views[0].orientation, {1, 0, 0});
      const auto up = rotate(views[0].orientation, {0, 1, 0});
      constexpr uint32_t material = maxFaces - 1;
      Material marker;
      marker.native[3] = 255;
      static_cast<Material *>(materialBuffer.mapped)[material] = marker;
      const auto point = [&](float x, float y) {
        Vec3 position = target;
        for (unsigned axis = 0; axis < 3; ++axis)
          position[axis] += right[axis] * x + up[axis] * y;
        return Vertex{position, {}, {0.55f, 1, 0.65f}, material};
      };
      const auto bar = [&](unsigned first, float x, float y) {
        reticle[first] = point(-x, -y);
        reticle[first + 1] = point(x, -y);
        reticle[first + 2] = point(-x, y);
        reticle[first + 3] = point(x, -y);
        reticle[first + 4] = point(x, y);
        reticle[first + 5] = point(-x, y);
      };
      bar(0, 0.018f, 0.006f);
      bar(6, 0.006f, 0.018f);
      std::memcpy(static_cast<Vertex *>(vertexBuffer.mapped) + vertices.size(),
                  reticle.data(), sizeof(reticle));
    }
    std::array<uint32_t, 2> image{};
    if (!offscreen()) for (size_t i = 0; i < image.size(); ++i) image[i] = xr.acquire(i);
    vkCheck(vkResetCommandBuffer(xr.commands, 0), "Reset VR command buffer");
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkCheck(vkBeginCommandBuffer(xr.commands, &begin), "Begin VR commands");
    for (size_t i = 0; i < image.size(); ++i) {
      const auto &target = targets[i];
      std::array<VkClearValue, 2> clear{};
      clear[0].color.float32[3] = 1;
      clear[1].depthStencil.depth = 1;
      VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      passBegin.renderPass = pass;
      passBegin.framebuffer = target.framebuffers.at(image[i]);
      passBegin.renderArea.extent = target.extent;
      passBegin.clearValueCount = clear.size();
      passBegin.pClearValues = clear.data();
      vkCmdBeginRenderPass(xr.commands, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
      const VkViewport viewport{0, 0, float(target.extent.width), float(target.extent.height), 0, 1};
      const VkRect2D scissor{{0, 0}, target.extent};
      vkCmdSetViewport(xr.commands, 0, 1, &viewport);
      vkCmdSetScissor(xr.commands, 0, 1, &scissor);
      vkCmdBindDescriptorSets(xr.commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelineLayout, 0, 1, &descriptor, 0, nullptr);
      const VkDeviceSize offset = 0;
      vkCmdBindVertexBuffers(xr.commands, 0, 1, &vertexBuffer.buffer, &offset);
      Anchor renderAnchor = anchor;
      if (playerCamera)
        renderAnchor.heading = yaw(2 * std::atan2(anchor.heading[1], anchor.heading[3]) + stickTurn);
      const auto matrix = eyeMatrix(renderAnchor, views[i]);
      vkCmdPushConstants(xr.commands, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                         0, sizeof(matrix), matrix.data());
      const auto draw = [&](VkPipeline pipeline, uint32_t from, uint32_t to) {
        if (from == to) return;
        vkCmdBindPipeline(xr.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(xr.commands, to - from, 1, from, 0);
      };
      draw(worldPipeline, 0, solidVertices);
      draw(translucentPipeline, solidVertices, worldVertices);
      draw(panelPipeline, worldVertices, vertices.size());
      if (showAim) {
        const auto pointerMatrix = eyeMatrix(Anchor{}, views[i]);
        vkCmdPushConstants(xr.commands, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(pointerMatrix), pointerMatrix.data());
        draw(panelPipeline, vertices.size(), vertices.size() + reticle.size());
      }
      vkCmdEndRenderPass(xr.commands);
      if (offscreen()) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = i * captureSize * captureSize * 4u;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {captureSize, captureSize, 1};
        vkCmdCopyImageToBuffer(xr.commands, target.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer, 1, &copy);
      }
    }
    if (offscreen()) {
      VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      hostRead.buffer = readback.buffer;
      hostRead.size = readback.size;
      vkCmdPipelineBarrier(xr.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead, 0, nullptr);
    }
    vkCheck(vkEndCommandBuffer(xr.commands), "End VR commands");
    vkCheck(vkResetFences(device(), 1, &fence), "Reset VR fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &xr.commands;
    vkCheck(vkQueueSubmit(xr.queue, 1, &submit, fence), "Submit both VR eyes");
    // Both images and the shared upload have the same ownership fence.
    vkCheck(vkWaitForFences(device(), 1, &fence, VK_TRUE, UINT64_MAX), "Wait for both VR eyes");
    if (!offscreen()) for (size_t i = 0; i < image.size(); ++i) xr.release(i);
  }

  void dump(const char *pose) {
    const auto *bytes = static_cast<const uint8_t *>(readback.mapped);
    for (size_t i = 0; i < 2; ++i) {
      std::ofstream file(directory / (std::string(pose) + (i ? "-right.ppm" : "-left.ppm")),
                         std::ios::binary);
      file << "P6\n" << captureSize << ' ' << captureSize << "\n255\n";
      for (uint32_t j = 0; j < captureSize * captureSize; ++j)
        file.write(reinterpret_cast<const char *>(bytes + (i * captureSize * captureSize + j) * 4), 3);
      if (!file) throw std::runtime_error("Could not write MGS eye capture");
    }
  }
};

Renderer::Renderer(GPU &gpu, const std::filesystem::path &output)
    : impl_(std::make_unique<Impl>(gpu, output)) {
  impl_->init();
}
Renderer::~Renderer() = default;

bool Renderer::beginFrame(input::VrInputSample &sample) {
  auto &state = *impl_;
  sample = {};
  if (state.offscreen()) {
    if (state.playerCamera && state.worldFrame) {
      const auto &player = *state.worldFrame->latch().mgsPlayerView;
      if (const char *heading = std::getenv("PS1_VR_HEADLESS_MOVE_YAW");
          heading && state.display.latchFrame >= 16028) {
        const auto stick = input::headRelativeStick(0, 1, std::strtof(heading, nullptr), player.padOrigin);
        sample.analogMovement = true;
        sample.x = stick[0];
        sample.y = stick[1];
        sample.facing = uint16_t(int32_t(std::lround(std::strtof(heading, nullptr) *
            (4096 / (2 * std::numbers::pi_v<float>)))) & 4095);
      }
      if (const char *heading = std::getenv("PS1_VR_HEADLESS_AIM_YAW")) {
        const float angle = std::strtof(heading, nullptr);
        const char *pitchValue = std::getenv("PS1_VR_HEADLESS_AIM_PITCH");
        const float pitch = pitchValue ? std::strtof(pitchValue, nullptr) : 0;
        const float horizontal = std::cos(pitch);
        games::mgs::setVrShotDirection({std::sin(angle) * horizontal, std::sin(pitch),
                                      std::cos(angle) * horizontal});
        const float halfYaw = (angle - state.baseHeading) * 0.5f;
        const float halfPitch = pitch * 0.5f;
        const Quaternion q{std::cos(halfYaw) * std::sin(halfPitch),
                           std::sin(halfYaw) * std::cos(halfPitch),
                           -std::sin(halfYaw) * std::sin(halfPitch),
                           std::cos(halfYaw) * std::cos(halfPitch)};
        state.pad.aimPose = {{q[0], q[1], q[2], q[3]}, {0.18f, -0.16f, -0.3f}};
        state.pad.aimValid = true;
        // The diagnostic fire input still goes through the original pad.
        sample.facing = uint16_t(int32_t(std::lround(angle *
            (4096 / (2 * std::numbers::pi_v<float>)))) & 4095);
        sample.aimPitch = int16_t(std::lround(-pitch *
            (4096 / (2 * std::numbers::pi_v<float>))));
      }
    }
    return true;
  }
  state.canDraw = state.xr.wait(state.eyes);
  const bool focused = state.canDraw && state.xr.state == XR_SESSION_STATE_FOCUSED;
  sample.buttons = state.pad.sample(state.xr, focused);
  const bool switchPressed = (sample.buttons & input::BTN_R3) != 0;
  if (switchPressed && !state.switchHeld) {
    state.firstPerson = !state.firstPerson;
    state.builtFrame = UINT64_MAX;
    state.playerEpoch = 0;
  }
  state.switchHeld = switchPressed;
  sample.buttons &= ~input::BTN_R3;
  if (state.firstPerson && state.playerCamera) {
    const bool trigger = sample.buttons & input::BTN_R2;
    sample.buttons &= ~input::BTN_R2;
    if (trigger) sample.buttons |= input::BTN_SQUARE;
    if (state.pad.grip > 0.65f) sample.buttons |= input::BTN_R2;
  }
  if (!focused || !state.playerCamera || !state.pad.aimValid)
    games::mgs::setVrShotDirection({});
  if (!state.canDraw) {
    state.xr.end(false);
    return false;
  }
  if (state.xr.needsAnchor) {
    state.anchor = recenter(state.eyes);
    // A standby pose is provisional until the game first gains focus.
    state.xr.needsAnchor = !focused;
  }
  if (focused && state.playerCamera && state.worldFrame) {
    const auto &player = *state.worldFrame->latch().mgsPlayerView;
    constexpr float turnSpeed = std::numbers::pi_v<float> / 2;
    const float period = std::clamp(float(state.xr.frameState.predictedDisplayPeriod) * 1e-9f,
                                    0.0f, 1.0f / 30);
    state.stickTurn += state.pad.turn * turnSpeed * period;
    state.stickTurn = std::remainder(state.stickTurn, 2 * std::numbers::pi_v<float>);
    const auto head = rotate(inverse(state.anchor.heading),
        rotate(state.eyes[0].orientation, {0, 0, -1}));
    if (std::hypot(head[0], head[2]) > 0.1f)
      state.headTurn = std::atan2(head[0], -head[2]);
    const float heading = state.baseHeading - state.stickTurn - state.headTurn;
    float right = state.pad.move.x, forward = state.pad.move.y;
    if (std::hypot(right, forward) < 0.25f) {
      right = float((sample.buttons & input::BTN_RIGHT) != 0) -
              float((sample.buttons & input::BTN_LEFT) != 0);
      forward = float((sample.buttons & input::BTN_UP) != 0) -
                float((sample.buttons & input::BTN_DOWN) != 0);
    }
    const auto stick = input::headRelativeStick(right, forward, heading, player.padOrigin);
    sample.analogMovement = true;
    sample.x = stick[0];
    sample.y = stick[1];
    sample.facing = uint16_t(int32_t(std::lround(heading *
        (4096 / (2 * std::numbers::pi_v<float>)))) & 4095);
    if (state.pad.aimValid) {
      const auto &q = state.pad.aimPose.orientation;
      const auto aim = rotate(inverse(state.anchor.heading),
                              rotate({q.x, q.y, q.z, q.w}, {0, 0, -1}));
      const float aimHeading = state.baseHeading - state.stickTurn - std::atan2(aim[0], -aim[2]);
      const float horizontal = std::hypot(aim[0], aim[2]);
      games::mgs::setVrShotDirection({std::sin(aimHeading) * horizontal, aim[1],
                                    std::cos(aimHeading) * horizontal});
      if (sample.buttons & input::BTN_SQUARE) {
        sample.facing = uint16_t(int32_t(std::lround(aimHeading *
            (4096 / (2 * std::numbers::pi_v<float>)))) & 4095);
        sample.aimPitch = int16_t(std::lround(-std::atan2(aim[1], horizontal) *
            (4096 / (2 * std::numbers::pi_v<float>))));
      }
    }
  }
  return focused;
}

void Renderer::present() {
  auto &state = *impl_;
  if (!state.offscreen() && !state.canDraw) return;
  state.updateScene();
  if (state.offscreen()) {
    if (state.captured || state.worldFaces == 0 || state.builtFrame < state.captureAt ||
        (state.firstPerson && !state.playerCamera))
      return;
    std::array<EyeView, 2> coincident{};
    state.drawEyes(coincident);
    state.dump("coincident");
    state.drawEyes(state.eyes);
    state.dump("forward");
    state.drawEyes(diagnosticEyes(0.45f));
    state.dump("turned");
    state.drawEyes(diagnosticEyes(0, 0.12f));
    state.dump("leaned");
    state.captured = true;
    std::cout << "[MGS VR] Captured four eye pairs at VBlank " << state.builtFrame
              << " from " << state.worldFaces << " original faces";
    if (state.playerCamera) {
      const auto &player = *state.worldFrame->latch().mgsPlayerView;
      std::cout << " in first person at (" << player.eye[0] << ", " << player.eye[1]
                << ", " << player.eye[2] << "), yaw " << player.yaw;
    }
    std::cout << '\n';
    return;
  }
  state.drawEyes(state.eyes);
  state.xr.end(true);
  state.canDraw = false;
  if (++state.submitted == 1)
    std::cout << "[MGS VR] First projection frame submitted\n";
}

void Renderer::rebase() {
  impl_->worldFrame.reset();
  impl_->builtFrame = impl_->revision = UINT64_MAX;
  impl_->lastWorld = 0;
  impl_->playerEpoch = 0;
  games::mgs::setVrShotDirection({});
}

bool Renderer::running() const { return !impl_->xr.finished && !impl_->captured; }
} // namespace ps1::gpu::vr
