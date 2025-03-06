#pragma once

#include <jvk.hpp>

namespace VkUtil {

bool loadShaderModule(const char *filePath, VkDevice device, VkShaderModule *outShaderModule);

struct PipelineBuilder {
    std::vector<VkPipelineShaderStageCreateInfo> _shaderStages;// Shader modules for different stages
    VkPipelineInputAssemblyStateCreateInfo _inputAssembly;     // Triangle topology
    VkPipelineRasterizationStateCreateInfo _rasterizer;        // Rasterization settings between vertex & frag shader
    VkPipelineColorBlendAttachmentState _colorBlendAttachment; // Color blending & attachment information (transparency)
    VkPipelineMultisampleStateCreateInfo _multisampling;       // MSAA
    VkPipelineLayout _pipelineLayout;                          // Pipeline layout (desriptors, etc)
    VkPipelineDepthStencilStateCreateInfo _depthStencil;       // Depth-testing & stencil configuration
    VkPipelineRenderingCreateInfo _renderingInfo;              // Holds attachment info for pipeline, passed via pNext
    VkFormat _colorAttachmentFormat;

    // Pipeline parameters we don't configure:
    // - VkPipelineVertexInputStateCreateInfo: vertex attribute input configuration; we use "vertex pulling" so don't need it
    // - VkPipelineTesselationStateCreateInfo: fixed tesselation; we don't use it
    // - VkPipelineViewportStateCreateInfo:    information about rendering viewport; we are using dynamic state for this
    // - renderPass, subpass:                  we use dynamic rendering, so we just attach _renderingInfo into pNext.

    // We set up VkPipelineDynamicStateCreateInfo in the buildPipeline method for dynamic scissor and viewport

    PipelineBuilder() { clear(); }
    void clear();
    void setShaders(VkShaderModule vertexShader, VkShaderModule fragmentShader);
    void setInputTopology(VkPrimitiveTopology topology);

    // Rasterizer state
    void setPolygonMode(VkPolygonMode mode);
    void setCullMode(VkCullModeFlags cullMode, VkFrontFace frontFace);

    // Multisampling
    void setMultiSamplingNone();

    // Blending
    void disableBlending();

    // Attachments
    void setColorAttachmentFormat(VkFormat format);
    void setDepthAttachmentFormat(VkFormat format);

    // Depth testing
    void disableDepthTest();

    VkPipeline buildPipeline(VkDevice device) const;
};

inline void PipelineBuilder::disableDepthTest() {
    _depthStencil.depthTestEnable       = VK_FALSE;
    _depthStencil.depthWriteEnable      = VK_FALSE;
    _depthStencil.depthCompareOp        = VK_COMPARE_OP_NEVER;
    _depthStencil.depthBoundsTestEnable = VK_FALSE;
    _depthStencil.stencilTestEnable     = VK_FALSE;
    _depthStencil.front                 = {};
    _depthStencil.back                  = {};
    _depthStencil.minDepthBounds        = 0.0f;
    _depthStencil.maxDepthBounds        = 1.0f;
}

}// namespace VkUtil