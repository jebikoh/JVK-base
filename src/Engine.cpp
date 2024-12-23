#include "Engine.hpp"

#include "vk/Commands.hpp"
#include "vk/Descriptors.hpp"
#include "vk/Image.hpp"
#include "vk/Pipeline.hpp"
#include "vk/Render.hpp"
#include "vk/Shaders.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <VkBootstrap.h>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>

#include <iostream>
#include <thread>

namespace jvk {

constexpr bool USE_VALIDATION_LAYERS = true;

void Engine::init() {
    initSDL();
    initVulkan();
    initSwapchain();
    initCommands();
    initSyncStructures();
    initDescriptors();
    initPipelines();
    initImGUI();
    initDummyData();

    isInit_ = true;
}

void Engine::destroy() {
    if (!isInit_) { return; }
    vkDeviceWaitIdle(context_.device);

    // Clean up frame command pools
    for (auto &frame: frames_) {
        frame.commandPool.destroy();
        frame.drawFence.destroy(context_);
        frame.drawSemaphore.destroy(context_);
        frame.swapchainSemaphore.destroy(context_);
        frame.deletionQueue.flush();
    }

    globalDeletionQueue_.flush();
    swapchain_.destroy(context_);
    context_.destroy();
    SDL_DestroyWindow(window_);
}

#pragma region Initialization
void Engine::initSDL() {
    SDL_Init(SDL_INIT_VIDEO);

    auto window_flags = (SDL_WindowFlags) (SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    window_           = SDL_CreateWindow(
            "JVK Engine",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            windowExtent_.width,
            windowExtent_.height,
            window_flags);
}

void Engine::initVulkan() {
    vkb::InstanceBuilder builder;

    auto vkbInstance = builder.set_app_name("JVK")
                               .request_validation_layers(USE_VALIDATION_LAYERS)
                               .use_default_debug_messenger()
                               .require_api_version(1, 3, 0)
                               .build();

    if (!vkbInstance) {
        std::cerr << "Failed to create Vulkan instance. Error: " << vkbInstance.error().message() << "\n";
        return;
    }

    vkb::Instance vkbInst = vkbInstance.value();
    context_.instance     = vkbInst.instance;
    context_.debug        = vkbInst.debug_messenger;

    SDL_Vulkan_CreateSurface(window_, context_.instance, &context_.surface);

    // Features
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = true;
    features13.synchronization2 = true;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing  = true;

    // Device
    vkb::PhysicalDeviceSelector selector{vkbInst};
    auto vkbPhysicalDeviceResult = selector
                                           .set_minimum_version(1, 3)
                                           .set_required_features_13(features13)
                                           .set_required_features_12(features12)
                                           .set_surface(context_.surface)
                                           .select();
    if (!vkbPhysicalDeviceResult) {
        std::cerr << "Failed to select physical device. Error: " << vkbPhysicalDeviceResult.error().message() << "\n";
        return;
    }
    const auto &vkbPhysicalDevice = vkbPhysicalDeviceResult.value();

    context_.physicalDevice = vkbPhysicalDevice.physical_device;

    vkb::DeviceBuilder deviceBuilder{vkbPhysicalDevice};
    vkb::Device vkbDevice = deviceBuilder.build().value();
    context_.device       = vkbDevice.device;

    // Queue
    graphicsQueue_.queue  = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    graphicsQueue_.family = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    // Allocator
    allocator_.init(context_);
    globalDeletionQueue_.push([&]() { allocator_.destroy(); });
}

void Engine::initImGUI() {
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
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

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType                      = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags                      = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets                    = 1000;
    pool_info.poolSizeCount              = (uint32_t) std::size(pool_sizes);
    pool_info.pPoolSizes                 = pool_sizes;

    VkDescriptorPool imguiPool;
    VK_CHECK(vkCreateDescriptorPool(context_, &pool_info, nullptr, &imguiPool));

    ImGui::CreateContext();

    ImGui_ImplSDL2_InitForVulkan(window_);

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance                  = context_;
    initInfo.PhysicalDevice            = context_;
    initInfo.Device                    = context_;
    initInfo.Queue                     = graphicsQueue_.queue;
    initInfo.DescriptorPool            = imguiPool;
    initInfo.MinImageCount             = 3;
    initInfo.ImageCount                = 3;
    initInfo.UseDynamicRendering       = true;

    initInfo.PipelineRenderingCreateInfo                         = {};
    initInfo.PipelineRenderingCreateInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchain_.imageFormat;

    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&initInfo);
    ImGui_ImplVulkan_CreateFontsTexture();

    globalDeletionQueue_.push([=, this]() {
        ImGui_ImplVulkan_Shutdown();
        vkDestroyDescriptorPool(context_, imguiPool, nullptr);
    });
}

void Engine::initSwapchain() {
    swapchain_ = Swapchain{context_, windowExtent_.width, windowExtent_.height};

    VkExtent3D drawImageExtent = {
            windowExtent_.width,
            windowExtent_.height,
            1};

    VkImageUsageFlags drawImageUsages{};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    drawImage_.init(context_, allocator_, VK_FORMAT_R16G16B16A16_SFLOAT, drawImageUsages, drawImageExtent, VK_IMAGE_ASPECT_COLOR_BIT);

    // Depth-image
    VkImageUsageFlags depthImageUsages{};
    depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    depthImage_.init(context_, allocator_, VK_FORMAT_D32_SFLOAT, depthImageUsages, drawImageExtent, VK_IMAGE_ASPECT_DEPTH_BIT);
    globalDeletionQueue_.push([=, this]() {
        depthImage_.destroy(context_, allocator_);
        drawImage_.destroy(context_, allocator_);
    });
}

void Engine::resizeSwapchain() {
    vkDeviceWaitIdle(context_);
    swapchain_.destroy(context_);

    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    windowExtent_ = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};

    swapchain_.init(context_, w, h);
    windowResize_ = false;
}

void Engine::initCommands() {
    for (auto &frame: frames_) {
        frame.commandPool.init(context_, graphicsQueue_.family, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
        frame.mainCommandBuffer = frame.commandPool.createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    }

    immediateBuffer_.init(context_, graphicsQueue_.family);
    globalDeletionQueue_.push([=, this]() {
        immediateBuffer_.destroy(context_);
    });
}

void Engine::initSyncStructures() {
    for (auto &frame: frames_) {
        frame.swapchainSemaphore.init(context_);
        frame.drawSemaphore.init(context_);
        frame.drawFence.init(context_, VK_FENCE_CREATE_SIGNALED_BIT);
    }
}

void Engine::initDescriptors() {
    std::vector<DescriptorAllocator::PoolSizeRatio> sizes =
            {
                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};

    descriptorAllocator_.initPool(context_, 10, sizes);

    // Compute layout
    {
        DescriptorLayoutBindings bindings;
        bindings.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        drawImageDescriptorLayout_ = bindings.build(context_, VK_SHADER_STAGE_COMPUTE_BIT);

        drawImageDescriptor_ = descriptorAllocator_.allocate(context_, drawImageDescriptorLayout_);

        DescriptorWriter writer;
        writer.writeImage(0, drawImage_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.updateSet(context_, drawImageDescriptor_);

        globalDeletionQueue_.push([&]() {
            descriptorAllocator_.destroyPool(context_);
            vkDestroyDescriptorSetLayout(context_, drawImageDescriptorLayout_, nullptr);
        });

        for (int i = 0; i < NUM_FRAMES; ++i) {
            std::vector<DynamicDescriptorAllocator::PoolSizeRatio> frameSizes = {
                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
            };

            frames_[i].descriptors = DynamicDescriptorAllocator{};
            frames_[i].descriptors.init(context_, 1000, frameSizes);

            globalDeletionQueue_.push([&, i] {
                frames_[i].descriptors.destroyPools(context_);
            });
        }
    }

    {
        DescriptorLayoutBindings builder;
        builder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        gpuSceneDataLayout_ = builder.build(context_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

        globalDeletionQueue_.push([&] {
            vkDestroyDescriptorSetLayout(context_, gpuSceneDataLayout_, nullptr);
        });
    }
}

void Engine::draw() {
    auto &frame = this->getCurrentFrame();
    frame.drawFence.wait(context_);
    frame.deletionQueue.flush();
    frame.descriptors.clearPools(context_);
    frame.drawFence.reset(context_);

    uint32_t swapchainImageIndex;
    if (const auto e = swapchain_.acquireNextImage(context_, frame.swapchainSemaphore, swapchainImageIndex); e == VK_ERROR_OUT_OF_DATE_KHR) {
        windowResize_ = true;
        return;
    }

    VkCommandBuffer cmd = getCurrentFrame().mainCommandBuffer;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    drawImageExtent_.height  = std::min(swapchain_.extent.height, drawImage_.extent_.width) * renderScale;
    drawImageExtent_.width = std::min(swapchain_.extent.width, drawImage_.extent_.width) * renderScale;

    beginCommandBuffer(cmd, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    transitionImage(cmd, drawImage_.image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    drawBackground(cmd);

    transitionImage(cmd, drawImage_.image_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // transitionImage(cmd, drawImage_.image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transitionImage(cmd, depthImage_.image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    drawGeometry(cmd);

    transitionImage(cmd, drawImage_.image_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionImage(cmd, swapchain_.images[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyImage(cmd, drawImage_.image_, swapchain_.images[swapchainImageIndex], drawImageExtent_, swapchain_.extent);

    transitionImage(cmd, swapchain_.images[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    drawUI(cmd, swapchain_.imageViews[swapchainImageIndex]);

    transitionImage(cmd, swapchain_.images[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    endCommandBuffer(cmd);

    auto waitSemaphore   = frame.swapchainSemaphore.submitInfo(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR);
    auto signalSemaphore = frame.drawSemaphore.submitInfo(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);

    submitCommandBuffer(graphicsQueue_.queue, cmd, &waitSemaphore, &signalSemaphore, frame.drawFence);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext              = nullptr;
    presentInfo.pSwapchains        = &swapchain_.swapchain;
    presentInfo.swapchainCount     = 1;
    presentInfo.pWaitSemaphores    = &frame.drawSemaphore.semaphore;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pImageIndices      = &swapchainImageIndex;
    if (const auto presentResult = vkQueuePresentKHR(graphicsQueue_.queue, &presentInfo); presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
        windowResize_ = true;
    }

    frameNumber_++;
}

void Engine::run() {
    SDL_Event e;
    bool quit = false;

    while (!quit) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) { quit = true; }
            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) { stopRendering_ = true; }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) { stopRendering_ = false; }
            }

            ImGui_ImplSDL2_ProcessEvent(&e);
        }
        if (stopRendering_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (windowResize_) {
            resizeSwapchain();
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ImGui::ShowDemoWindow();

        if (ImGui::Begin("Settings")) {
            ImGui::SliderFloat("Render Scale", &renderScale, 0.3f, 1.0f);
        }
        ImGui::End();

        ImGui::Render();

        draw();
    }
}

void Engine::drawBackground(VkCommandBuffer cmd) const {
    constexpr VkClearColorValue clearValue   = {{35.0f / 255.0f, 43.0f / 255.0f, 43.0f / 255.0f, 1.0f}};
    const VkImageSubresourceRange clearRange = imageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdClearColorImage(cmd, drawImage_.image_, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}

void Engine::drawUI(VkCommandBuffer cmd, VkImageView targetImageView) const {
    auto colorAttachment       = create::attachmentInfo(targetImageView, nullptr);
    VkRenderingInfo renderInfo = create::renderingInfo(swapchain_.extent, &colorAttachment, nullptr);

    vkCmdBeginRendering(cmd, &renderInfo);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

void Engine::initPipelines() {
    initMeshPipeline();
}

void Engine::drawGeometry(VkCommandBuffer cmd) {
    VkRenderingAttachmentInfo colorAttachment = create::attachmentInfo(drawImage_.view_, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = create::depthAttachmentInfo(depthImage_.view_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo                = create::renderingInfo(drawImageExtent_, &colorAttachment, &depthAttachment);
    vkCmdBeginRendering(cmd, &renderInfo);

    // Viewport & scissor
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline_.pipeline);
    VkViewport viewport = {};
    viewport.x          = 0;
    viewport.y          = 0;
    viewport.width      = drawImageExtent_.width;
    viewport.height     = drawImageExtent_.height;
    viewport.minDepth   = 0.0f;
    viewport.maxDepth   = 1.0f;

    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor      = {};
    scissor.offset.x      = 0;
    scissor.offset.y      = 0;
    scissor.extent.width  = drawImageExtent_.width;
    scissor.extent.height = drawImageExtent_.height;

    vkCmdSetScissor(cmd, 0, 1, &scissor);

    GPUDrawPushConstants pushConstants;

    // Draw scene
    glm::mat4 view = glm::translate(glm::vec3{0, 0, -5});
    glm::mat4 proj = glm::perspective(glm::radians(70.0f), (float) drawImageExtent_.width / (float) drawImageExtent_.height, 0.1f , 10000.0f);
    proj[1][1] *= -1;
    pushConstants.worldMatrix = proj * view;
    // pushConstants.worldMatrix = glm::mat4{1.0f};
    pushConstants.vertexBufferAddress = scene[2]->gpuBuffers.vertexBufferAddress;

    // BROKEN
    // Buffer sceneDataBuffer = allocator_.createBuffer(sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    // getCurrentFrame().deletionQueue.push([=, this]() {
    //     sceneDataBuffer.destroy(allocator_);
    // });
    // auto *sceneUniformData = reinterpret_cast<GPUSceneData *>(sceneDataBuffer.getDeviceAddress(context_));
    // *sceneUniformData = sceneData_;
    //
    // VkDescriptorSet frameDescriptor = getCurrentFrame().descriptors.allocate(context_, gpuSceneDataLayout_);
    // DescriptorWriter writer;
    // writer.writeBuffer(0, sceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    // writer.updateSet(context_, frameDescriptor);

    vkCmdPushConstants(cmd, meshPipeline_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &pushConstants);
    vkCmdBindIndexBuffer(cmd, scene[2]->gpuBuffers.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, scene[2]->surfaces[0].count, 1, scene[2]->surfaces[0].startIndex, 0, 0);

    vkCmdEndRendering(cmd);
}

GPUMeshBuffers Engine::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices) {
    // Create buffers
    const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
    const size_t indexBufferSize  = indices.size() * sizeof(uint32_t);

    GPUMeshBuffers meshBuffer;

    meshBuffer.vertexBuffer = allocator_.createBuffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    VkBufferDeviceAddressInfo deviceAddressInfo = {};
    deviceAddressInfo.sType                     = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    deviceAddressInfo.buffer                    = meshBuffer.vertexBuffer.buffer;
    meshBuffer.vertexBufferAddress              = vkGetBufferDeviceAddress(context_, &deviceAddressInfo);

    meshBuffer.indexBuffer = allocator_.createBuffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    // Upload data
    Buffer staging = allocator_.createBuffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    void *data;
    vmaMapMemory(allocator_.allocator, staging.allocation, &data);
    memcpy(data, vertices.data(), vertexBufferSize);
    memcpy((char *) data + vertexBufferSize, indices.data(), indexBufferSize);
    vmaUnmapMemory(allocator_.allocator, staging.allocation);

    immediateBuffer_.submit(context_, graphicsQueue_.queue, [&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{0};
        vertexCopy.dstOffset = 0;
        vertexCopy.srcOffset = 0;
        vertexCopy.size      = vertexBufferSize;
        vkCmdCopyBuffer(cmd, staging.buffer, meshBuffer.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{0};
        indexCopy.dstOffset = 0;
        indexCopy.srcOffset = vertexBufferSize;
        indexCopy.size      = indexBufferSize;
        vkCmdCopyBuffer(cmd, staging.buffer, meshBuffer.indexBuffer.buffer, 1, &indexCopy);
    });

    staging.destroy(allocator_);
    return meshBuffer;
}

void Engine::initMeshPipeline() {
    VkShaderModule fragShader;
    VkShaderModule vertShader;

    if (!loadShaderModule("../shaders/colored_triangle_mesh.vert.spv", context_, &vertShader)) {
        std::cerr << "Failed to load vertex shader" << std::endl;
    }

    if (!loadShaderModule("../shaders/colored_triangle.frag.spv", context_, &fragShader)) {
        std::cerr << "Failed to load fragment shader" << std::endl;
    }

    VkPushConstantRange bufferRange = {};
    bufferRange.offset              = 0;
    bufferRange.size                = sizeof(GPUDrawPushConstants);
    bufferRange.stageFlags          = VK_SHADER_STAGE_VERTEX_BIT;

    VkPipelineLayoutCreateInfo layoutInfo = create::pipelineLayout();
    layoutInfo.pPushConstantRanges        = &bufferRange;
    layoutInfo.pushConstantRangeCount     = 1;

    VK_CHECK(vkCreatePipelineLayout(context_, &layoutInfo, nullptr, &meshPipeline_.layout));

    PipelineBuilder builder;
    builder.pipelineLayout_ = meshPipeline_.layout;
    builder.setShaders(vertShader, fragShader);
    builder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.setPolygonMode(VK_POLYGON_MODE_FILL);
    builder.setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    builder.setMultisamplingNone();
    builder.disableBlending();
    builder.enableDepthTest(true, VK_COMPARE_OP_LESS_OR_EQUAL);
    builder.setColorAttachmentFormat(drawImage_.format_);
    builder.setDepthFormat(depthImage_.format_);
    meshPipeline_.pipeline = builder.buildPipeline(context_);

    vkDestroyShaderModule(context_, fragShader, nullptr);
    vkDestroyShaderModule(context_, vertShader, nullptr);

    globalDeletionQueue_.push([=, this]() {
        vkDestroyPipelineLayout(context_, meshPipeline_.layout, nullptr);
        vkDestroyPipeline(context_, meshPipeline_.pipeline, nullptr);
    });
}

void Engine::initDummyData() {
    scene = loadMeshes(this, "../assets/basicmesh.glb").value();
    globalDeletionQueue_.push([=, this]() {
        for (const auto &mesh: scene) {
            mesh->destroy(allocator_);
        }
    });
}

#pragma endregion

}// namespace jvk