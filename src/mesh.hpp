#pragma once
#include "fastgltf/types.hpp"
#include "material.hpp"

#include <filesystem>
#include <jvk.hpp>
#include <jvk/descriptor.hpp>

namespace jvk {
struct Image;
}

class JVKEngine;

/**
 * Interleaved vertex data
 */
struct Vertex {
    glm::vec3 position;
    float uv_x;
    glm::vec3 normal;
    float uv_y;
    glm::vec4 color;
};

/**
 * Contains the index/vertex buffers for a mesh
 */
struct GPUMeshBuffers {
    jvk::Buffer indexBuffer;
    jvk::Buffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress;
};

/**
 * Global scene data, best passed via UB
 */
struct GPUSceneData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewProj;
    glm::vec4 ambientColor;
    glm::vec4 sunlightDirection;
    glm::vec4 sunlightColor;
};

/**
 * Global push constants. Contains:
 *  - worldMatrix: The world matrix transform
 *  - vertexBuffer: The address of the vertex buffer
 */
struct GPUDrawPushConstants {
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
};

/**
 * An individual surface of a mesh, specified buy start index,
 * face (triangle) count, and material.
 *
 * In other graphics programs, this may just be referred to as
 * a "mesh" or "submesh".
 */
struct Surface {
    uint32_t startIndex;
    uint32_t count;
    std::shared_ptr<GLTFMaterial> material;
};

/**
 * A complete mesh asset. Contains:
 *  - name: The name of the mesh; will be defaulted if missing
 *  - surfaces: A list of surfaces that make up the mesh (submeshes)
 *  - meshBuffers: The GPU buffers for the mesh
 */
struct MeshAsset {
    std::string name;

    std::vector<Surface> surfaces;
    GPUMeshBuffers meshBuffers;
};

/**
 * A flattened render object, which contains all the required
 * data and pointers for the engine to render a mesh.
 */
struct RenderObject {
    uint32_t indexCount;
    uint32_t firstIndex;
    VkBuffer indexBuffer;

    MaterialInstance *material;

    glm::mat4 transform;
    VkDeviceAddress vertexBufferAddress;
};

/**
 * A context for drawing, containing a list of opaque and transparent
 * surfaces to render. Should be updated per frame.
 */
struct DrawContext {
    std::vector<RenderObject> opaqueSurfaces;
    std::vector<RenderObject> transparentSurfaces;
};

/**
 * A generic renderable object. Can be a mesh, a node, or a scene.
 *
 * TODO: look to replace this with an ECS or something
 */
class IRenderable {
    virtual void draw(const glm::mat4 &topMatrix, DrawContext &ctx) = 0;
};

/**
 * A generic node in a scene graph. Contains a local and world transform.
 *
 * The transforms can be updated by calling refreshTransform with the parent
 * Calling draw() will simply call draw() on all children.
 */
struct Node : public IRenderable {
    std::weak_ptr<Node> parent;
    std::vector<std::shared_ptr<Node>> children;

    glm::mat4 localTransform;
    glm::mat4 worldTransform;

    void refreshTransform(const glm::mat4 &parentMatrix) {
        worldTransform = parentMatrix * localTransform;
        for (const auto &child : children) {
            child->refreshTransform(worldTransform);
        }
    }

    virtual void draw(const glm::mat4 &topMatrix, DrawContext &ctx) override {
        for (const auto &child : children) {
            child->draw(topMatrix, ctx);
        }
    }
};

/**
 * A node that contains a mesh asset. Calling draw() will
 * generate a RenderObject and pass it into the draw context
 */
struct MeshNode : public Node {
    std::shared_ptr<MeshAsset> mesh;

    virtual void draw(const glm::mat4 &topMatrix, DrawContext &ctx) override;
};

/**
 * Represents a fully loaded glTF 2.0 file, which see view as a scene.
 *
 * Calling draw() on this will process all of it's child MeshNodes
 */
struct LoadedGLTF : public IRenderable {
    std::unordered_map<std::string, std::shared_ptr<MeshAsset>> meshes;
    std::unordered_map<std::string, std::shared_ptr<Node>> nodes;
    std::unordered_map<std::string, jvk::Image> images;
    std::unordered_map<std::string, std::shared_ptr<GLTFMaterial>> materials;

    std::vector<std::shared_ptr<Node>> topNodes;
    std::vector<VkSampler> samplers;

    jvk::DynamicDescriptorAllocator descriptorPool;

    jvk::Buffer materialDataBuffer;

    JVKEngine *engine;

    ~LoadedGLTF() { destroy(); };

    virtual void draw(const glm::mat4 &topMatrix, DrawContext &ctx);

private:
    void destroy();
};

/**
 * Will load a full glTF 2.0 file from the given path.
 * @param engine The engine to load the glTF into
 * @param filePath The path to the glTF file
 * @return A shared pointer to the loaded glTF file
 */
std::optional<std::shared_ptr<LoadedGLTF>> loadGLTF(JVKEngine *engine, std::filesystem::path filePath);