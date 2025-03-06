#include <engine.hpp>
#include <jvk.hpp>
#include <vk_init.hpp>
#include <vk_pipelines.hpp>
#include <vk_util.hpp>

#include <SDL.h>
#include <SDL_vulkan.h>

#include "VkBootstrap.h"

#include <chrono>
#include <thread>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>

constexpr bool JVK_USE_VALIDATION_LAYERS = true;

JVKEngine *loadedEngine = nullptr;

JVKEngine &JVKEngine::get() {
    return *loadedEngine;
}

void JVKEngine::init() {
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags windowFlags = (SDL_WindowFlags) (SDL_WINDOW_VULKAN);

    _window = SDL_CreateWindow(
            "JVK",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            _windowExtent.width,
            _windowExtent.height,
            windowFlags);

    initVulkan();
    initSwapchain();
    initCommands();
    initSyncStructures();
    initDescriptors();
    initPipelines();
    initImgui();

    _isInitialized = true;
}

void JVKEngine::cleanup() {
    if (_isInitialized) {
        vkDeviceWaitIdle(_device);

        // Frame data
        for (int i = 0; i < JVK_NUM_FRAMES; ++i) {
            vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr);

            // Frame sync
            vkDestroyFence(_device, _frames[i]._renderFence, nullptr);
            vkDestroySemaphore(_device, _frames[i]._renderSemaphore, nullptr);
            vkDestroySemaphore(_device, _frames[i]._swapchainSemaphore, nullptr);

            _frames[i]._deletionQueue.flush();
        }

        // ImGui
        ImGui_ImplVulkan_Shutdown();
        vkDestroyDescriptorPool(_device, _imguiPool, nullptr);

        // Immediate command pool
        vkDestroyCommandPool(_device, _immCommandPool, nullptr);
        vkDestroyFence(_device, _immFence, nullptr);

        _globalDeletionQueue.flush();

        // Pipelines
        vkDestroyPipelineLayout(_device, _trianglePipelineLayout, nullptr);
        vkDestroyPipeline(_device, _trianglePipeline, nullptr);

        vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
        vkDestroyPipeline(_device, computeEffects[0].pipeline, nullptr);
        vkDestroyPipeline(_device, computeEffects[1].pipeline, nullptr);

        // Descriptors
        _globalDescriptorAllocator.destroyPool(_device);
        vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);

        // Draw image
        vkDestroyImageView(_device, _drawImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);

        // VMA
        vmaDestroyAllocator(_allocator);

        // Swapchain
        destroySwapchain();

        // API
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);
        vkb::destroy_debug_utils_messenger(_instance, _debugMessenger);
        vkDestroyInstance(_instance, nullptr);
        SDL_DestroyWindow(_window);
    }

    loadedEngine = nullptr;
}

void JVKEngine::draw() {
    // Wait and reset render fence
    VK_CHECK(vkWaitForFences(_device, 1, &getCurrentFrame()._renderFence, true, 1000000000));
    VK_CHECK(vkResetFences(_device, 1, &getCurrentFrame()._renderFence));

    // Request an image from swapchain
    uint32_t swapchainImageIndex;
    VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, getCurrentFrame()._swapchainSemaphore, nullptr, &swapchainImageIndex));

    // Reset the command buffer
    VkCommandBuffer cmd = getCurrentFrame()._mainCommandBuffer;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    _drawExtent.width  = _drawImage.imageExtent.width;
    _drawExtent.height = _drawImage.imageExtent.height;

    // Start the command buffer
    VkCommandBufferBeginInfo cmdBeginInfo = VkInit::commandBufferBegin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    // Transition draw image to general
    VkUtil::transitionImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    ComputeEffect &effect = computeEffects[currentComputeEffect];

    // Bind compute pipeline & descriptors
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipelineLayout, 0, 1, &_drawImageDescriptors, 0, nullptr);

    // Push constants for compute
    vkCmdPushConstants(cmd, _gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect.data);

    // Draw compute
    vkCmdDispatch(cmd, std::ceil(_drawExtent.width / 16.0f), std::ceil(_drawExtent.height / 16.0f), 1);

    // Transition draw image for render pass
    VkUtil::transitionImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    drawGeometry(cmd);

    // Transition draw image to transfer source
    VkUtil::transitionImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Transition swapchain image to transfer destination
    VkUtil::transitionImage(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Copy image to from draw image to swapchain
    VkUtil::copyImageToImage(cmd, _drawImage.image, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);

    // Transition swapchain to attachment optimal
    VkUtil::transitionImage(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Draw UI
    drawImgui(cmd, _swapchainImageViews[swapchainImageIndex]);

    // Transition swapchain for presentation
    VkUtil::transitionImage(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // End command buffer
    VK_CHECK(vkEndCommandBuffer(cmd));

    // Submit buffer
    // srcStageMask set to COLOR_ATTACHMENT_OUTPUT_BIT to wait for color attachment output (waiting for swapchain image)
    // dstStageMask set to ALL_GRAPHICS_BIT to signal that all graphics stages are done
    VkCommandBufferSubmitInfo cmdInfo = VkInit::commandBufferSubmit(cmd);
    VkSemaphoreSubmitInfo waitInfo    = VkInit::semaphoreSubmit(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, getCurrentFrame()._swapchainSemaphore);
    VkSemaphoreSubmitInfo signalInfo  = VkInit::semaphoreSubmit(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, getCurrentFrame()._renderSemaphore);
    VkSubmitInfo2 submit              = VkInit::submit(&cmdInfo, &signalInfo, &waitInfo);

    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, getCurrentFrame()._renderFence));

    // Present
    VkPresentInfoKHR presentInfo   = {};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext              = nullptr;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &_swapchain;
    presentInfo.pWaitSemaphores    = &getCurrentFrame()._renderSemaphore;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pImageIndices      = &swapchainImageIndex;
    VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &presentInfo));

    _frameNumber++;
}

void JVKEngine::run() {
    SDL_Event e;
    bool bQuit = false;

    while (!bQuit) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) bQuit = true;

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    _stopRendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    _stopRendering = false;
                }
            }

            ImGui_ImplSDL2_ProcessEvent(&e);
        }

        if (_stopRendering) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();

        ImGui::NewFrame();

        if (ImGui::Begin("computeEffects")) {

            ComputeEffect &selected = computeEffects[currentComputeEffect];

            ImGui::Text("Selected effect: ", selected.name);

            ImGui::SliderInt("Effect Index", &currentComputeEffect, 0, computeEffects.size() - 1);

            ImGui::InputFloat4("data1", (float *) &selected.data.data1);
            ImGui::InputFloat4("data2", (float *) &selected.data.data2);
            ImGui::InputFloat4("data3", (float *) &selected.data.data3);
            ImGui::InputFloat4("data4", (float *) &selected.data.data4);
        }
        ImGui::End();

        ImGui::Render();

        draw();
    }
}

void JVKEngine::initVulkan() {
    // CREATE INSTANCE
    vkb::InstanceBuilder builder;
    auto vkbInstanceResult = builder.set_app_name("JVK")
                                     .request_validation_layers(JVK_USE_VALIDATION_LAYERS)
                                     .use_default_debug_messenger()
                                     .require_api_version(1, 3, 0)
                                     .build();

    if (!vkbInstanceResult) {
        fmt::println("Failed to create Vulkan instance. Error: {}", vkbInstanceResult.error().message());
        abort();
    }

    vkb::Instance vkbInstance = vkbInstanceResult.value();

    _instance       = vkbInstance.instance;
    _debugMessenger = vkbInstance.debug_messenger;

    // CREATE SURFACE
    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

    // 1.3 FEATURES
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = true;
    features13.synchronization2 = true;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing  = true;

    // PHYSICAL DEVICE
    vkb::PhysicalDeviceSelector physicalDeviceBuilder{vkbInstance};
    auto vkbPhysicalDeviceResult = physicalDeviceBuilder.set_minimum_version(1, 3)
                                           .set_required_features_13(features13)
                                           .set_required_features_12(features12)
                                           .set_surface(_surface)
                                           .select();

    if (!vkbPhysicalDeviceResult) {
        fmt::println("Failed to select physical device. Error: {}", vkbPhysicalDeviceResult.error().message());
        abort();
    }

    vkb::PhysicalDevice vkbPhysicalDevice = vkbPhysicalDeviceResult.value();

    // DEVICE
    vkb::DeviceBuilder deviceBuilder{vkbPhysicalDevice};
    vkb::Device vkbDevice = deviceBuilder.build().value();
    _device               = vkbDevice.device;
    _chosenGPU            = vkbPhysicalDevice.physical_device;

    // QUEUE
    _graphicsQueue       = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    // VMA
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice         = _chosenGPU;
    allocatorInfo.device                 = _device;
    allocatorInfo.instance               = _instance;
    allocatorInfo.flags                  = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &_allocator);
}

void JVKEngine::initSwapchain() {
    createSwapchain(_windowExtent.width, _windowExtent.height);

    // CREATE DRAW IMAGE
    VkExtent3D drawImageExtent = {
            _windowExtent.width,
            _windowExtent.height,
            1};

    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;// 16-bit float image
    _drawImage.imageExtent = drawImageExtent;

    VkImageUsageFlags drawImageUsages = {};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;    // Copy from image
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;    // Copy to image
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;         // Allow compute shader to write
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;// Graphics pipeline

    VkImageCreateInfo drawImageInfo = VkInit::image(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    // VMA_MEMORY_USAGE_GPU_ONLY: the texture will never be accessed from the CPU
    // VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT: GPU exclusive memory flag, guarantees that the memory is on the GPU
    VmaAllocationCreateInfo drawImageAllocInfo = {};
    drawImageAllocInfo.usage                   = VMA_MEMORY_USAGE_GPU_ONLY;
    drawImageAllocInfo.requiredFlags           = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vmaCreateImage(_allocator, &drawImageInfo, &drawImageAllocInfo, &_drawImage.image, &_drawImage.allocation, nullptr);

    VkImageViewCreateInfo imageViewInfo = VkInit::imageView(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_CHECK(vkCreateImageView(_device, &imageViewInfo, nullptr, &_drawImage.imageView));
}

void JVKEngine::initCommands() {
    // COMMAND POOL
    // Indicate that buffers should be individually resettable
    VkCommandPoolCreateFlags flags          = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPoolCreateInfo commandPoolInfo = VkInit::commandPool(_graphicsQueueFamily, flags);

    // COMMAND BUFFERS
    for (int i = 0; i < JVK_NUM_FRAMES; ++i) {
        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

        VkCommandBufferAllocateInfo cmdAllocInfo = VkInit::commandBuffer(_frames[i]._commandPool);

        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    }

    // IMMEDIATE BUFFERS
    VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_immCommandPool));
    VkCommandBufferAllocateInfo cmdAllocInfo = VkInit::commandBuffer(_immCommandPool, 1);
    VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immCommandBuffer));
}

void JVKEngine::initSyncStructures() {
    // FENCE
    // Start signaled to wait on the first frame
    VkFenceCreateInfo fenceCreateInfo = VkInit::fence(VK_FENCE_CREATE_SIGNALED_BIT);

    // SEMAPHORE
    VkSemaphoreCreateInfo semaphoreCreateInfo = VkInit::semaphore();

    for (int i = 0; i < JVK_NUM_FRAMES; ++i) {
        VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));

        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._renderSemaphore));
    }

    // IMMEDIATE SUBMIT FENCE
    VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));
}

void JVKEngine::createSwapchain(uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder swapchainBuilder{_chosenGPU, _device, _surface};
    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
                                          .set_desired_format(
                                                  VkSurfaceFormatKHR{
                                                          .format     = _swapchainImageFormat,
                                                          .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                                          .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                                          .set_desired_extent(width, height)
                                          .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                                          .build()
                                          .value();
    _swapchainExtent     = vkbSwapchain.extent;
    _swapchain           = vkbSwapchain.swapchain;
    _swapchainImages     = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void JVKEngine::destroySwapchain() {
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    for (int i = 0; i < _swapchainImageViews.size(); ++i) {
        vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    }
}

void JVKEngine::drawBackground(VkCommandBuffer cmd) {
    VkClearColorValue clearValue;
    float flash = std::abs(std::sin(_frameNumber / 120.0f));
    clearValue  = {{0.0f, 0.0f, flash, 1.0f}};

    VkImageSubresourceRange clearRange = VkInit::imageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);

    vkCmdClearColorImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}

void JVKEngine::initDescriptors() {
    // POOL
    std::vector<DescriptorAllocator::PoolSizeRatio> sizes = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    _globalDescriptorAllocator.initPool(_device, 10, sizes);

    // LAYOUTS
    {
        DescriptorLayoutBuilder builder;
        builder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    // SETS
    // Allocate draw image descriptor
    _drawImageDescriptors = _globalDescriptorAllocator.allocate(_device, _drawImageDescriptorLayout);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfo.imageView   = _drawImage.imageView;

    VkWriteDescriptorSet drawImageWrite{};
    drawImageWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawImageWrite.pNext           = nullptr;
    drawImageWrite.dstBinding      = 0;
    drawImageWrite.dstSet          = _drawImageDescriptors;
    drawImageWrite.descriptorCount = 1;
    drawImageWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    drawImageWrite.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(_device, 1, &drawImageWrite, 0, nullptr);
}

void JVKEngine::initPipelines() {
    initBackgroundPipelines();
    initTrianglePipeline();
}

void JVKEngine::initBackgroundPipelines() {
    // PIPELINE LAYOUT
    // Pass an aray of descriptor set layouts, push constants, etc
    VkPushConstantRange pushConstant{};
    pushConstant.offset     = 0;
    pushConstant.size       = sizeof(ComputePushConstants);
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkPipelineLayoutCreateInfo layout{};
    layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pNext                  = nullptr;
    layout.pSetLayouts            = &_drawImageDescriptorLayout;
    layout.setLayoutCount         = 1;
    layout.pPushConstantRanges    = &pushConstant;
    layout.pushConstantRangeCount = 1;
    VK_CHECK(vkCreatePipelineLayout(_device, &layout, nullptr, &_gradientPipelineLayout));

    // PIPELINE STAGES (AND SHADERS)
    VkShaderModule gradientShader;
    if (!VkUtil::loadShaderModule("../shaders/gradient_pc.comp.spv", _device, &gradientShader)) {
        fmt::print("Error when building gradient compute shader \n");
    }

    VkShaderModule skyShader;
    if (!VkUtil::loadShaderModule("../shaders/sky.comp.spv", _device, &skyShader)) {
        fmt::println("Error when building sky compute shader");
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.pNext  = nullptr;
    stageInfo.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = gradientShader;
    stageInfo.pName  = "main";

    // CREATE GRADIENT PIPELINE
    VkComputePipelineCreateInfo computeInfo{};
    computeInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computeInfo.pNext  = nullptr;
    computeInfo.layout = _gradientPipelineLayout;
    computeInfo.stage  = stageInfo;

    ComputeEffect gradient;
    gradient.layout     = _gradientPipelineLayout;
    gradient.name       = "gradient";
    gradient.data       = {};
    gradient.data.data1 = glm::vec4(1, 0, 0, 1);
    gradient.data.data2 = glm::vec4(0, 0, 1, 1);

    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &gradient.pipeline));

    // CREATE SKY PIPELINE
    computeInfo.stage.module = skyShader;

    ComputeEffect sky;
    sky.layout     = _gradientPipelineLayout;
    sky.name       = "sky";
    sky.data       = {};
    sky.data.data1 = glm::vec4(0.1, 0.2, 0.4, 0.97);
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &sky.pipeline));

    computeEffects.push_back(gradient);
    computeEffects.push_back(sky);

    vkDestroyShaderModule(_device, gradientShader, nullptr);
    vkDestroyShaderModule(_device, skyShader, nullptr);
}

void JVKEngine::immediateSubmit(std::function<void(VkCommandBuffer cmd)> &&function) {
    // Reset fence & buffer
    VK_CHECK(vkResetFences(_device, 1, &_immFence));
    VK_CHECK(vkResetCommandBuffer(_immCommandBuffer, 0));

    // Create and start buffer
    VkCommandBuffer cmd               = _immCommandBuffer;
    VkCommandBufferBeginInfo cmdBegin = VkInit::commandBufferBegin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBegin));

    // Record immediate submit commands
    function(cmd);

    // End buffer
    VK_CHECK(vkEndCommandBuffer(cmd));

    // Submit and wait for fence
    VkCommandBufferSubmitInfo cmdInfo = VkInit::commandBufferSubmit(cmd);
    VkSubmitInfo2 submit              = VkInit::submit(&cmdInfo, nullptr, nullptr);
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, _immFence));
    VK_CHECK(vkWaitForFences(_device, 1, &_immFence, true, 9999999999));
}

void JVKEngine::initImgui() {
    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
                                        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
                                        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 1000;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    poolInfo.pPoolSizes    = poolSizes;

    VK_CHECK(vkCreateDescriptorPool(_device, &poolInfo, nullptr, &_imguiPool));

    ImGui::CreateContext();
    ImGui_ImplSDL2_InitForVulkan(_window);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance            = _instance;
    initInfo.PhysicalDevice      = _chosenGPU;
    initInfo.Device              = _device;
    initInfo.Queue               = _graphicsQueue;
    initInfo.DescriptorPool      = _imguiPool;
    initInfo.MinImageCount       = 3;
    initInfo.ImageCount          = 3;
    initInfo.UseDynamicRendering = true;

    initInfo.PipelineRenderingCreateInfo                         = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    initInfo.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_swapchainImageFormat;

    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&initInfo);
    ImGui_ImplVulkan_CreateFontsTexture();
}

void JVKEngine::drawImgui(VkCommandBuffer cmd, VkImageView targetImageView) {
    // Setup color attachment for render pass
    VkRenderingAttachmentInfo colorAttachment = VkInit::renderingAttachment(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo                = VkInit::rendering(_swapchainExtent, &colorAttachment, nullptr);

    // Render
    vkCmdBeginRendering(cmd, &renderInfo);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

void JVKEngine::initTrianglePipeline() {
    // LOAD SHADER MODULES
    VkShaderModule triangleFragShader;
    if (!VkUtil::loadShaderModule("../shaders/colored_triangle.frag.spv", _device, &triangleFragShader)) {
        fmt::print("Error when building triangle fragment shader module");
    }

    VkShaderModule triangleVertShader;
    if (!VkUtil::loadShaderModule("../shaders/colored_triangle.vert.spv", _device, &triangleVertShader)) {
        fmt::print("Error when building triangle vertex shader module");
    }

    // CREATE PIPELINE LAYOUT
    VkPipelineLayoutCreateInfo piplineLayoutInfo = VkInit::pipelineLayout();
    VK_CHECK(vkCreatePipelineLayout(_device, &piplineLayoutInfo, nullptr, &_trianglePipelineLayout));

    // CREATE PIPELINE
    VkUtil::PipelineBuilder pipelineBuilder;
    pipelineBuilder._pipelineLayout = _trianglePipelineLayout;
    pipelineBuilder.setShaders(triangleVertShader, triangleFragShader);
    pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.setMultiSamplingNone();
    pipelineBuilder.disableBlending();
    pipelineBuilder.disableDepthTest();
    pipelineBuilder.setColorAttachmentFormat(_drawImage.imageFormat);
    pipelineBuilder.setDepthAttachmentFormat(VK_FORMAT_UNDEFINED);
    _trianglePipeline = pipelineBuilder.buildPipeline(_device);

    // CLEAN UP SHADER MODULES
    vkDestroyShaderModule(_device, triangleVertShader, nullptr);
    vkDestroyShaderModule(_device, triangleFragShader, nullptr);
}

void JVKEngine::drawGeometry(VkCommandBuffer cmd) {
    // SETUP RENDER PASS
    VkRenderingAttachmentInfo colorAttachment = VkInit::renderingAttachment(_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderingInfo             = VkInit::rendering(_drawExtent, &colorAttachment, nullptr);

    // BEGIN RENDER PASS
    vkCmdBeginRendering(cmd, &renderingInfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _trianglePipeline);

    // VIEWPORT
    VkViewport viewport{};
    viewport.x        = 0;
    viewport.y        = 0;
    viewport.width    = _drawExtent.width;
    viewport.height   = _drawExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // SCISSOR
    VkRect2D scissor{};
    scissor.offset.x      = 0;
    scissor.offset.y      = 0;
    scissor.extent.width  = _drawExtent.width;
    scissor.extent.height = _drawExtent.height;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // DRAW 3 VERTICES
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}
