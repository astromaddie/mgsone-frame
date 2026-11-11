#pragma once
#include "eye_view.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ps1::gpu::vr {
inline void xrCheck(XrResult result, const char *call) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

inline void vkCheck(VkResult result, const char *call) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

constexpr auto stereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrExtent2Di extent{};
    std::vector<XrSwapchainImageVulkan2KHR> images;
};

struct FrameRuntime {
    XrInstance xr = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    std::array<Eye, 2> eyes;
    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    bool begun = false, active = false, located = false, finished = false;
    bool needsAnchor = true;
    bool frameController = false;

    ~FrameRuntime() {
        if (device) vkDeviceWaitIdle(device);
        if (begun) {
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = frameState.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(session, &end);
        }
        for (const auto &eye : eyes) if (eye.swapchain) xrDestroySwapchain(eye.swapchain);
        if (space) xrDestroySpace(space);
        if (session) xrDestroySession(session);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (xr) xrDestroyInstance(xr);
    }

    template <typename F> F proc(const char *name) const {
        PFN_xrVoidFunction function = nullptr;
        xrCheck(xrGetInstanceProcAddr(xr, name, &function), name);
        if (!function) throw std::runtime_error(std::string("Unavailable: ") + name);
        return reinterpret_cast<F>(function);
    }

    void open(const char *title, bool enableFrameController = false) {
        const char *frameExtension = "XR_VALVE_frame_controller_interaction";
        if (enableFrameController) {
            uint32_t count = 0;
            xrCheck(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "Count OpenXR extensions");
            std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
            xrCheck(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "Read OpenXR extensions");
            frameController = std::any_of(extensions.begin(), extensions.end(), [&](const auto &item) {
                return std::strcmp(item.extensionName, frameExtension) == 0;
            });
        }
        const char *extensions[]{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, frameExtension};
        XrInstanceCreateInfo xrInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(xrInfo.applicationInfo.applicationName, title);
        xrInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        xrInfo.enabledExtensionCount = frameController ? 2 : 1;
        xrInfo.enabledExtensionNames = extensions;
        xrCheck(xrCreateInstance(&xrInfo, &xr), "xrCreateInstance (XR_KHR_vulkan_enable2)");
        XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
        xrCheck(xrGetInstanceProperties(xr, &runtime), "xrGetInstanceProperties");
        std::cout << "OpenXR runtime: " << runtime.runtimeName << '\n';
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        xrCheck(xrGetSystem(xr, &systemInfo, &system), "xrGetSystem");

        XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        xrCheck(proc<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR")
            (xr, system, &requirements), "xrGetVulkanGraphicsRequirements2KHR");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = xrInfo.applicationInfo.applicationName;
        app.apiVersion = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                                            XR_VERSION_MINOR(requirements.minApiVersionSupported), 0);
        VkInstanceCreateInfo vkInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        vkInfo.pApplicationInfo = &app;
        XrVulkanInstanceCreateInfoKHR instanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        instanceInfo.systemId = system;
        instanceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        instanceInfo.vulkanCreateInfo = &vkInfo;
        VkResult result = VK_SUCCESS;
        xrCheck(proc<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR")
            (xr, &instanceInfo, &instance, &result), "xrCreateVulkanInstanceKHR");
        vkCheck(result, "vkCreateInstance");

        XrVulkanGraphicsDeviceGetInfoKHR gpuInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        gpuInfo.systemId = system;
        gpuInfo.vulkanInstance = instance;
        xrCheck(proc<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR")
            (xr, &gpuInfo, &physical), "xrGetVulkanGraphicsDevice2KHR");
        VkPhysicalDeviceProperties gpu{};
        vkGetPhysicalDeviceProperties(physical, &gpu);
        std::cout << "Vulkan GPU: " << gpu.deviceName << ", API "
                  << VK_API_VERSION_MAJOR(app.apiVersion) << '.' << VK_API_VERSION_MINOR(app.apiVersion) << '\n';
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        const auto graphics = std::find_if(families.begin(), families.end(), [](const auto &f) {
            return f.queueCount && (f.queueFlags & VK_QUEUE_GRAPHICS_BIT);
        });
        if (graphics == families.end()) throw std::runtime_error("No Vulkan graphics queue");
        family = uint32_t(graphics - families.begin());
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo createDevice{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createDevice.queueCreateInfoCount = 1;
        createDevice.pQueueCreateInfos = &queueInfo;
        XrVulkanDeviceCreateInfoKHR deviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        deviceInfo.systemId = system;
        deviceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        deviceInfo.vulkanPhysicalDevice = physical;
        deviceInfo.vulkanCreateInfo = &createDevice;
        xrCheck(proc<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR")
            (xr, &deviceInfo, &device, &result), "xrCreateVulkanDeviceKHR");
        vkCheck(result, "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);

        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = instance;
        binding.physicalDevice = physical;
        binding.device = device;
        binding.queueFamilyIndex = family;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        sessionInfo.systemId = system;
        xrCheck(xrCreateSession(xr, &sessionInfo, &session), "xrCreateSession");
        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        reference.poseInReferenceSpace.orientation.w = 1.0f;
        xrCheck(xrCreateReferenceSpace(session, &reference, &space), "xrCreateReferenceSpace");

        createCommands();
    }

    void openOffscreen() {
        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
                                    "MGS Stereo Capture", 0, nullptr, 0, VK_API_VERSION_1_0};
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        vkCheck(vkCreateInstance(&create, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        vkCheck(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Count Vulkan devices");
        std::vector<VkPhysicalDevice> devices(count);
        vkCheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "List Vulkan devices");
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
                physical = candidate;
                break;
            }
        }
        if (!physical && !devices.empty()) physical = devices.front();
        if (!physical) throw std::runtime_error("No Vulkan physical device");
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        const auto graphics = std::find_if(families.begin(), families.end(), [](const auto &f) {
            return f.queueCount && (f.queueFlags & VK_QUEUE_GRAPHICS_BIT);
        });
        if (graphics == families.end()) throw std::runtime_error("No Vulkan graphics queue");
        family = uint32_t(graphics - families.begin());
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        vkCheck(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        createCommands();
    }

    void createCommands() {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        vkCheck(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo bufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        bufferInfo.commandPool = pool;
        bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        bufferInfo.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(device, &bufferInfo, &commands), "vkAllocateCommandBuffers");
    }

    void swapchains(uint32_t maxEyeSize) {
        uint32_t count = 0;
        xrCheck(xrEnumerateEnvironmentBlendModes(xr, system, stereo, 0, &count, nullptr), "Count blend modes");
        std::vector<XrEnvironmentBlendMode> modes(count);
        xrCheck(xrEnumerateEnvironmentBlendModes(xr, system, stereo, count, &count, modes.data()), "Read blend modes");
        if (std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes.end())
            throw std::runtime_error("Opaque stereo blend mode not offered");

        xrCheck(xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "Count swapchain formats");
        std::vector<int64_t> formats(count);
        xrCheck(xrEnumerateSwapchainFormats(session, count, &count, formats.data()), "Read swapchain formats");
        std::cout << "Swapchain formats:";
        for (const auto format : formats) std::cout << ' ' << format;
        std::cout << '\n';
        constexpr std::array preferred{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
        const auto format = std::find_if(preferred.begin(), preferred.end(), [&](VkFormat f) {
            return std::find(formats.begin(), formats.end(), f) != formats.end();
        });
        if (format == preferred.end()) throw std::runtime_error("No supported SDR swapchain format");

        colorFormat = *format;
        std::array<XrViewConfigurationView, 2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        xrCheck(xrEnumerateViewConfigurationViews(xr, system, stereo, views.size(), &count, views.data()), "Read stereo views");
        if (count != eyes.size()) throw std::runtime_error("Expected two stereo views");
        for (size_t i = 0; i < eyes.size(); ++i) {
            auto &eye = eyes[i];
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            info.format = *format;
            info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
            info.width = std::min(views[i].recommendedImageRectWidth, maxEyeSize);
            info.height = std::min(views[i].recommendedImageRectHeight, maxEyeSize);
            eye.extent = {int32_t(info.width), int32_t(info.height)};
            xrCheck(xrCreateSwapchain(session, &info, &eye.swapchain), "xrCreateSwapchain");
            xrCheck(xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr), "Count swapchain images");
            eye.images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
            xrCheck(xrEnumerateSwapchainImages(eye.swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader *>(eye.images.data())), "Read swapchain images");
            std::cout << "Eye " << i << ": " << info.width << 'x' << info.height << ", VkFormat "
                      << info.format << ", " << count << " images\n";
        }
    }

    void poll() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result;
        while ((result = xrPollEvent(xr, &event)) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) finished = true;
            if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) needsAnchor = true;
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto &change = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
                if (change.session == session) {
                    state = change.state;
                    std::cout << "OpenXR session state: " << int(state) << '\n';
                    if (state == XR_SESSION_STATE_READY && !active) {
                        const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, stereo};
                        xrCheck(xrBeginSession(session, &begin), "xrBeginSession");
                        active = true;
                    }
                    if (state == XR_SESSION_STATE_STOPPING && active) {
                        xrCheck(xrEndSession(session), "xrEndSession");
                        active = false;
                    }
                    if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING)
                        finished = true;
                }
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        xrCheck(result, "xrPollEvent");
    }

    bool wait(std::array<EyeView, 2> &eyesOut) {
        poll();
        located = false;
        if (!active || finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return false;
        }
        const XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        const XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        xrCheck(xrWaitFrame(session, &waitInfo, &frameState), "xrWaitFrame");
        xrCheck(xrBeginFrame(session, &beginInfo), "xrBeginFrame");
        begun = true;
        if (!frameState.shouldRender) return false;
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = stereo;
        locate.displayTime = frameState.predictedDisplayTime;
        locate.space = space;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t count = 0;
        xrCheck(xrLocateViews(session, &locate, &viewState, views.size(), &count, views.data()), "xrLocateViews");
        constexpr auto valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        if (count != views.size() || (viewState.viewStateFlags & valid) != valid) return false;
        for (size_t i = 0; i < views.size(); ++i) {
            const auto &v = views[i];
            const auto &p = v.pose.position;
            const auto &q = v.pose.orientation;
            eyesOut[i] = {{p.x, p.y, p.z}, {q.x, q.y, q.z, q.w}, std::tan(v.fov.angleLeft),
                          std::tan(v.fov.angleRight), std::tan(v.fov.angleDown), std::tan(v.fov.angleUp)};
        }
        located = true;
        return true;
    }

    uint32_t acquire(size_t eye) {
        uint32_t index = 0;
        const XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        xrCheck(xrAcquireSwapchainImage(eyes[eye].swapchain, &acquireInfo, &index), "xrAcquireSwapchainImage");
        const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        xrCheck(xrWaitSwapchainImage(eyes[eye].swapchain, &wait), "xrWaitSwapchainImage");
        return index;
    }

    void release(size_t eye) {
        const XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrCheck(xrReleaseSwapchainImage(eyes[eye].swapchain, &releaseInfo), "xrReleaseSwapchainImage");
    }

    void end(bool submit) {
        if (!begun) return;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frameState.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        std::array<XrCompositionLayerProjectionView, 2> projection{};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const auto *base = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
        if (submit && located) {
            for (size_t i = 0; i < views.size(); ++i) {
                projection[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projection[i].pose = views[i].pose;
                projection[i].fov = views[i].fov;
                projection[i].subImage.swapchain = eyes[i].swapchain;
                projection[i].subImage.imageRect.extent = eyes[i].extent;
            }
            layer.space = space;
            layer.viewCount = projection.size();
            layer.views = projection.data();
            end.layerCount = 1;
            end.layers = &base;
        }
        begun = false;
        xrCheck(xrEndFrame(session, &end), "xrEndFrame");
    }
};
} // namespace ps1::gpu::vr
