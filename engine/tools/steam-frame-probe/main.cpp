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

namespace {
void xrCheck(XrResult result, const char *call) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

void vkCheck(VkResult result, const char *call) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

constexpr auto stereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrExtent2Di extent{};
    std::vector<XrSwapchainImageVulkan2KHR> images;
};

struct Probe {
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

    ~Probe() {
        if (device) vkDeviceWaitIdle(device);
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

    void open() {
        const char *extension = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
        XrInstanceCreateInfo xrInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(xrInfo.applicationInfo.applicationName, "MGS Frame Probe");
        xrInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        xrInfo.enabledExtensionCount = 1;
        xrInfo.enabledExtensionNames = &extension;
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

    void swapchains() {
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
        constexpr std::array preferred{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                       VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
        const auto format = std::find_if(preferred.begin(), preferred.end(), [&](VkFormat f) {
            return std::find(formats.begin(), formats.end(), f) != formats.end();
        });
        if (format == preferred.end()) throw std::runtime_error("No supported SDR swapchain format");

        std::array<XrViewConfigurationView, 2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        xrCheck(xrEnumerateViewConfigurationViews(xr, system, stereo, views.size(), &count, views.data()), "Read stereo views");
        if (count != eyes.size()) throw std::runtime_error("Expected two stereo views");
        for (size_t i = 0; i < eyes.size(); ++i) {
            auto &eye = eyes[i];
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            info.format = *format;
            info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
            info.width = std::min(views[i].recommendedImageRectWidth, 512u);
            info.height = std::min(views[i].recommendedImageRectHeight, 512u);
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

    void clear() {
        std::array<uint32_t, 2> index{};
        const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        for (size_t i = 0; i < eyes.size(); ++i) {
            xrCheck(xrAcquireSwapchainImage(eyes[i].swapchain, &acquire, &index[i]), "xrAcquireSwapchainImage");
            xrCheck(xrWaitSwapchainImage(eyes[i].swapchain, &wait), "xrWaitSwapchainImage");
        }

        vkCheck(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer");
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkCheck(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
        for (size_t i = 0; i < eyes.size(); ++i) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = eyes[i].images.at(index[i]).image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            const VkClearColorValue colour{{i == 0 ? 1.0f : 0.0f, 0.0f, i == 1 ? 1.0f : 0.0f, 1.0f}};
            vkCmdClearColorImage(commands, barrier.image, barrier.newLayout, &colour, 1, &barrier.subresourceRange);
            barrier.oldLayout = barrier.newLayout;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        vkCheck(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        vkCheck(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        // A blocking wait is sufficient for this short diagnostic. The game
        // renderer will track its own in-flight work and resources.
        vkCheck(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        for (const auto &eye : eyes)
            xrCheck(xrReleaseSwapchainImage(eye.swapchain, &release), "xrReleaseSwapchainImage");
    }

    bool frame() {
        const XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        xrCheck(xrWaitFrame(session, &wait, &frameState), "xrWaitFrame");
        const XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        xrCheck(xrBeginFrame(session, &begin), "xrBeginFrame");
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frameState.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        std::array<XrCompositionLayerProjectionView, 2> projection{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const auto *base = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
        if (frameState.shouldRender) {
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = stereo;
            locate.displayTime = frameState.predictedDisplayTime;
            locate.space = space;
            XrViewState state{XR_TYPE_VIEW_STATE};
            std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
            uint32_t count = 0;
            xrCheck(xrLocateViews(session, &locate, &state, views.size(), &count, views.data()), "xrLocateViews");
            constexpr auto valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            if (count == eyes.size() && (state.viewStateFlags & valid) == valid) {
                clear();
                for (size_t i = 0; i < eyes.size(); ++i) {
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
        }
        xrCheck(xrEndFrame(session, &end), "xrEndFrame");
        return end.layerCount != 0;
    }

    unsigned run() {
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + 12s;
        bool running = false;
        bool requestedExit = false;
        unsigned submitted = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            XrResult result;
            while ((result = xrPollEvent(xr, &event)) == XR_SUCCESS) {
                if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) return submitted;
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    const auto &change = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
                    std::cout << "OpenXR session state " << change.state << '\n';
                    if (change.state == XR_SESSION_STATE_READY) {
                        const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, stereo};
                        xrCheck(xrBeginSession(session, &begin), "xrBeginSession");
                        running = true;
                    } else if (change.state == XR_SESSION_STATE_STOPPING) {
                        xrCheck(xrEndSession(session), "xrEndSession");
                        return submitted;
                    } else if (change.state == XR_SESSION_STATE_EXITING || change.state == XR_SESSION_STATE_LOSS_PENDING) {
                        return submitted;
                    }
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }
            xrCheck(result, "xrPollEvent");
            if (running) {
                if (frame()) ++submitted;
                if (!requestedExit && (submitted >= 90 || std::chrono::steady_clock::now() >= deadline - 2s)) {
                    xrCheck(xrRequestExitSession(session), "xrRequestExitSession");
                    requestedExit = true;
                }
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return submitted;
    }
};
} // namespace

int main() {
    std::cout << std::unitbuf;
    try {
        Probe probe;
        probe.open();
        probe.swapchains();
        const auto frames = probe.run();
        std::cout << "Submitted " << frames << " red-left / blue-right projection frames.\n";
        if (!frames) std::cout << "No renderable tracked views; check session focus and standby.\n";
        return frames ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
