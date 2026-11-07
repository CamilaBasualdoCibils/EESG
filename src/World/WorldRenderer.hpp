#pragma once

#include "World/Entity/MeshRender.hpp"
#include "World/Entity/Transform.hpp"
#include "World/World.hpp"
#include "World/Terrain/TerrainMeshPool.hpp"
#include <Lighthouse/Engine/Assets/Mesh.hpp>
#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <Lighthouse/Engine/Rendering/Renderer.hpp>
#include <algorithm>
#include <array>

namespace EESG
{
// Temporary forward renderer with a fixed camera and embedded Phong shaders.
class WorldRenderer final : public Lighthouse::IRenderTechnique
{
public:
  WorldRenderer(Lighthouse::Renderer* renderer, World* world,
                UVec2 resolution = {1280, 720})
      : m_renderer(renderer), m_world(world), m_resolution(resolution),
        m_terrainPool(*renderer->GetRenderDevice())
  {
    SetRenderResolution(resolution);
  }
  ~WorldRenderer() override
  {
    auto& device = *m_renderer->GetRenderDevice();
    if (m_pipeline || !m_draws.empty())
      device.WaitIdle();
    ReleaseDraws();
    if (m_pipeline)
      device.DestroyPipeline(m_pipeline);
    if (m_vertexShader)
      device.DestroyModule(m_vertexShader);
    if (m_fragmentShader)
      device.DestroyModule(m_fragmentShader);
    if (m_pipelineLayout)
      device.DestroyPipelineLayout(m_pipelineLayout);
    if (m_setLayout)
      device.DestroyDescriptorSetLayout(m_setLayout);
  }

  void BuildGraph(Lighthouse::TechniqueBuildContext& context) override
  {
    using namespace Lighthouse;
    auto& device = context.device;
    if (!m_pipeline)
      CreatePipeline(device);
    if (!m_draws.empty())
      device.WaitIdle();
    ReleaseDraws();
    const auto size = m_resolution;
    struct Geometry
    {
      RenderGraphResource vertices, indices, uniform;
      RenderMeshHandle mesh;
      entt::entity entity;
      DescriptorSetHandle set;
    };
    std::vector<Geometry> geometry;
    auto entities = m_world->Registry().view<Transform, MeshRender>();
    for (auto entity : entities)
    {
      const auto meshHandle = entities.get<MeshRender>(entity).mesh;
      const auto* mesh = m_renderer->GetRenderMesh(meshHandle);
      if (!mesh || m_renderer->GetRenderMeshUploadStatus(meshHandle) !=
                       RenderMeshUploadStatus::Ready)
        continue;
      if (mesh->layout.bindings.size() != 1 ||
          mesh->vertexBuffers.size() != 1 || !mesh->indexBuffer ||
          mesh->indexBuffer->format != IndexFormat::UInt32 ||
          mesh->topology != PrimitiveTopology::TriangleList)
        throw std::logic_error(
            "The temporary WorldRenderer expects indexed Mesh3D geometry");
      // The manager may accept other layouts, but this shader has fixed inputs.
      if (mesh->layout.bindings.front().stride != sizeof(Mesh3DVertex))
        throw std::logic_error(
            "The temporary WorldRenderer expects Mesh3DVertex");
      m_draws.emplace_back();
      auto& draw = m_draws.back();
      const auto model = entities.get<Transform>(entity).GetMatrix();
      const auto bytes = std::as_bytes(std::span(&model, 1));
      draw.uniform = device.CreateBuffer({.size = bytes.size(),
                                          .usage = BufferUsage::Uniform,
                                          .initialData = bytes});
      draw.set = device.CreateDescriptorSet({.layout = m_setLayout});
      const std::array writes{
          DescriptorWrite{.binding = 0,
                          .resource = DescriptorBufferInfo{
                              .buffer = draw.uniform, .range = bytes.size()}}};
      device.UpdateDescriptorSet(draw.set, writes);
      const auto vertexBuffer = mesh->vertexBuffers.front().buffer;
      const auto indexBuffer = mesh->indexBuffer->buffer;
      geometry.push_back(
          {context.graph.Import(
               "World vertices",
               ImportedRenderGraphBuffer::Desc{
                   .size = device.GetBufferDesc(vertexBuffer)->size,
                   .usage = BufferUsage::Vertex},
               ImportedRenderGraphBuffer{vertexBuffer}),
           context.graph.Import(
               "World indices",
               ImportedRenderGraphBuffer::Desc{
                   .size = device.GetBufferDesc(indexBuffer)->size,
                   .usage = BufferUsage::Index},
               ImportedRenderGraphBuffer{indexBuffer}),
           context.graph.Import(
               "Object model matrix",
               ImportedRenderGraphBuffer::Desc{.size = bytes.size(),
                                               .usage = BufferUsage::Uniform},
               ImportedRenderGraphBuffer{draw.uniform}),
           meshHandle, entity, draw.set});
    }
    m_terrainPool.Rebuild(m_world->GetTerrain());
    std::vector<RenderGraphResource> terrainBuffers;
    DescriptorSetHandle terrainSet{};
    if (m_terrainPool.Vertices())
    {
      m_draws.emplace_back();
      auto& draw = m_draws.back();
      const Mat4 identity(1.0f);
      const auto bytes = std::as_bytes(std::span(&identity, 1));
      draw.uniform = device.CreateBuffer({.size = bytes.size(),
          .usage = BufferUsage::Uniform, .initialData = bytes});
      draw.set = device.CreateDescriptorSet({.layout = m_setLayout});
      const std::array writes{DescriptorWrite{.binding = 0,
          .resource = DescriptorBufferInfo{.buffer = draw.uniform, .range = bytes.size()}}};
      device.UpdateDescriptorSet(draw.set, writes);
      terrainSet = draw.set;
      for (auto buffer : {m_terrainPool.Vertices(), m_terrainPool.Indices(),
                          m_terrainPool.Indirect(), draw.uniform})
      {
        const auto desc = device.GetBufferDesc(buffer);
        terrainBuffers.push_back(context.graph.Import("Terrain buffer",
            ImportedRenderGraphBuffer::Desc{.size = desc->size, .usage = desc->usage},
            ImportedRenderGraphBuffer{buffer}));
      }
    }
    struct PassData
    {
      RenderGraphResource color, depth;
      std::vector<Geometry> geometry;
      std::vector<RenderGraphResource> terrain;
      DescriptorSetHandle terrainSet{};
    };
    const auto& output = context.graph.AddPass<PassData>(
        "World Phong",
        [size, geometry, terrainBuffers, terrainSet](RenderGraph::Builder& builder, PassData& data)
        {
          data.color = builder.Create<RenderGraphImage>(
              "World color",
              {.width = size.x,
               .height = size.y,
               .format = ImageFormat::RGBA8,
               .usage = {ImageUsage::ColorAttachment,
                         ImageUsage::TransferSource, ImageUsage::Sampled}});
          data.color = builder.Write(data.color);
          data.depth = builder.Create<RenderGraphImage>(
              "World depth", {.width = size.x,
                              .height = size.y,
                              .format = ImageFormat::Depth32,
                              .usage = {ImageUsage::DepthAttachment}});
          data.depth = builder.Write(data.depth);
          data.terrainSet = terrainSet;
          for (auto buffer : terrainBuffers) data.terrain.push_back(builder.Read(buffer));
          data.geometry = geometry;
          for (auto& draw : data.geometry)
          {
            draw.vertices = builder.Read(draw.vertices);
            draw.indices = builder.Read(draw.indices);
            draw.uniform = builder.Read(draw.uniform);
          }
        },
        [this, size](const PassData& data, FrameGraphPassResources& resources,
                     RenderGraphExecutePassContext& execute)
        {
          const auto& target = resources.get<RenderGraphImage>(data.color);
          const auto& depth = resources.get<RenderGraphImage>(data.depth);
          auto& commands = *execute.commandList;
          const auto width = std::min(size.x, size.y * 16u / 9u);
          const auto height = std::min(size.y, size.x * 9u / 16u);
          commands.Begin();
          commands.BeginRendering(
              {.colorAttachment = target.viewHandle,
               .depthAttachment = depth.viewHandle,
               .clearColor = true,
               .clearColorValue = {0.025f, 0.035f, 0.05f},
               .clearDepth = true,
               .width = width,
               .height = height,
               .offsetX = static_cast<std::int32_t>((size.x - width) / 2),
               .offsetY = static_cast<std::int32_t>((size.y - height) / 2)});
          commands.BindPipeline(m_pipeline);
          for (const auto& draw : data.geometry)
          {
            auto& registry = m_world->Registry();
            if (!registry.valid(draw.entity) ||
                !registry.all_of<Transform, MeshRender>(draw.entity))
              continue;
            const auto* mesh = m_renderer->GetRenderMesh(draw.mesh);
            if (!mesh)
              continue;
            const auto model = registry.get<Transform>(draw.entity).GetMatrix();
            // UpdateBuffer is a synchronous host upload. Previous GPU use
            // completes before writing, and these commands have not yet been
            // submitted.
            m_renderer->GetRenderDevice()->UpdateBuffer(
                resources.get<ImportedRenderGraphBuffer>(draw.uniform).handle,
                0, std::as_bytes(std::span(&model, 1)));
            commands.BindDescriptorSet(0, draw.set);
            commands.BindVertexBuffer(
                resources.get<ImportedRenderGraphBuffer>(draw.vertices).handle,
                mesh->layout.bindings.front().stride);
            commands.BindIndexBuffer(
                resources.get<ImportedRenderGraphBuffer>(draw.indices).handle);
            commands.DrawIndexedRange(
                mesh->indexCount,
                static_cast<std::uint32_t>(mesh->indexBuffer->offset /
                                           sizeof(std::uint32_t)),
                static_cast<std::int32_t>(
                    mesh->vertexBuffers.front().offset /
                    mesh->layout.bindings.front().stride));
          }
          if (data.terrainSet)
          {
            commands.BindDescriptorSet(0, data.terrainSet);
            m_terrainPool.Draw(commands);
          }
          commands.EndRendering();
          commands.End();
        });
    m_output = output.color;
  }

  // Call Renderer::Compile() after changing resolution to rebuild the images.
  void SetRenderResolution(UVec2 resolution)
  {
    if (!resolution.x || !resolution.y)
      throw std::invalid_argument("World render resolution must be nonzero");
    m_resolution = resolution;
  }
  UVec2 GetRenderResolution() const noexcept
  {
    return m_resolution;
  }
  Lighthouse::RenderGraphResource GetOutput() const noexcept
  {
    return m_output;
  }

private:
  void CreatePipeline(Lighthouse::IRenderDevice& device)
  {
    using namespace Lighthouse;
    constexpr std::string_view vertexSource = R"glsl(#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(set = 0, binding = 0, std140) uniform ObjectData { mat4 model; } objectData;
void main()
{
    mat4 model = objectData.model;
    const mat4 view = mat4(
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0,-3.6, 1);
    // 45 degree perspective, 16:9, near 0.1, far 100.
    const mat4 projection = mat4(
        1.357995, 0, 0, 0,
        0, 2.414214, 0, 0,
        0, 0,-1.001001,-1,
        0, 0,-0.1001001, 0);

    worldPosition = (model * vec4(position, 1.0)).xyz;
    worldNormal = transpose(inverse(mat3(model))) * normal;
    gl_Position = projection * view * vec4(worldPosition, 1.0);
}
)glsl";
    constexpr std::string_view fragmentSource = R"glsl(#version 450
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 color;
void main()
{
    vec3 n = normalize(worldNormal);
    vec3 l = normalize(vec3(-0.25, 1.0, 0.45)); // Toward overhead light.
    vec3 v = normalize(vec3(0.0, 0.0, 3.6) - worldPosition);
    float diffuse = max(dot(n, l), 0.0);
    float specular = diffuse > 0.0
        ? pow(max(dot(reflect(-l, n), v), 0.0), 32.0) : 0.0;
    vec3 albedo = vec3(0.42, 0.48, 0.36);
    vec3 lit = albedo * (0.12 + 0.88 * diffuse) + vec3(0.35 * specular);
    color = vec4(pow(lit, vec3(1.0 / 2.2)), 1.0);
}
)glsl";
    m_setLayout = device.CreateDescriptorSetLayout(
        {.bindings = {{.binding = 0,
                       .type = DescriptorType::UniformBuffer,
                       .count = 1,
                       .stages = ShaderStageFlags::Vertex}}});
    m_pipelineLayout = device.CreatePipelineLayout({.sets = {m_setLayout}});
    m_vertexShader = device.CreateShaderModule(
        ShaderCompiler::Compile(vertexSource, {.stage = ShaderStage::Vertex}));
    m_fragmentShader = device.CreateShaderModule(ShaderCompiler::Compile(
        fragmentSource, {.stage = ShaderStage::Fragment}));
    m_pipeline = device.CreatePipeline(
        {.vertexShader = m_vertexShader,
         .fragmentShader = m_fragmentShader,
         .vertexLayout =
             {.bindings = {{.binding = 0, .stride = sizeof(Mesh3DVertex)}},
              .attributes = {{.location = 0,
                              .binding = 0,
                              .format = VertexFormat::Float3,
                              .offset = offsetof(Mesh3DVertex, position)},
                             {.location = 1,
                              .binding = 0,
                              .format = VertexFormat::Float3,
                              .offset = offsetof(Mesh3DVertex, normal)}}},
         .colorFormats = {ImageFormat::RGBA8},
         .depthFormat = ImageFormat::Depth32,
         .layout = m_pipelineLayout});
  }
  void ReleaseDraws()
  {
    auto& device = *m_renderer->GetRenderDevice();
    for (auto& draw : m_draws)
    {
      if (draw.set)
        device.DestroyDescriptorSet(draw.set);
      if (draw.uniform)
        device.DestroyBuffer(draw.uniform);
    }
    m_draws.clear();
  }
  struct DrawResources
  {
    Lighthouse::BufferHandle uniform{};
    Lighthouse::DescriptorSetHandle set{};
  };
  Lighthouse::Renderer* m_renderer;
  World* m_world;
  UVec2 m_resolution;
  Lighthouse::RenderGraphResource m_output{Lighthouse::kInvalidGraphResource};
  Lighthouse::DescriptorSetLayoutHandle m_setLayout{};
  Lighthouse::PipelineLayoutHandle m_pipelineLayout{};
  Lighthouse::ShaderModuleHandle m_vertexShader{}, m_fragmentShader{};
  Lighthouse::GraphicsPipelineHandle m_pipeline{};
  std::vector<DrawResources> m_draws;
  TerrainMeshPool m_terrainPool;
};
} // namespace EESG
