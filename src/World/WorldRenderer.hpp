#pragma once

#include "Materials/Materials.hpp"
#include "World/Entity/Light.hpp"
#include "World/Entity/MeshRender.hpp"
#include "World/Entity/Transform.hpp"
#include "World/LightVolumes.hpp"
#include "World/Terrain/TerrainMeshPool.hpp"
#include "World/World.hpp"
#include <Lighthouse/Engine/Assets/Mesh.hpp>
#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <Lighthouse/Engine/Rendering/Renderer.hpp>
#include <algorithm>
#include <array>
#include <boost/describe/enum_to_string.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace EESG
{
class WorldRenderer final : public Lighthouse::IRenderTechnique
{
public:
  WorldRenderer(Lighthouse::Renderer* renderer, World* world,
                UVec2 resolution = {1280, 720})
      : m_renderer(renderer), m_world(world), m_resolution(resolution),
        m_terrainPool(*renderer->GetRenderDevice()),
        m_materials(*renderer->GetRenderDevice())
  {
    SetRenderResolution(resolution);
  }
  ~WorldRenderer() override
  {
    auto& device = *m_renderer->GetRenderDevice();
    device.WaitIdle();
    ReleaseDraws();
    for (auto mesh : m_volumes)
      if (mesh)
        m_renderer->DestroyRenderMesh(mesh);
    if (m_outputPipeline)
      device.DestroyPipeline(m_outputPipeline);
    if (m_outputVertex)
      device.DestroyModule(m_outputVertex);
    if (m_outputFragment)
      device.DestroyModule(m_outputFragment);
    if (m_outputLayout)
      device.DestroyPipelineLayout(m_outputLayout);
    if (m_outputSetLayout)
      device.DestroyDescriptorSetLayout(m_outputSetLayout);
    if (m_sampler)
      device.DestroySampler(m_sampler);
  }
  void BuildGraph(Lighthouse::TechniqueBuildContext& context) override
  {
    using namespace Lighthouse;
    auto& device = context.device;
    device.WaitIdle();
    ReleaseDraws();
    if (!m_sampler)
      m_sampler = device.CreateSampler({});
    if (!m_terrainMaterial)
      m_terrainMaterial =
          m_materials.Load("materials/Terrain.material.json")->Instantiate();
    EnsureLightVolumes();
    if (!m_outputPipeline)
      CreateOutputPipeline(device);
    const auto size = m_resolution;
    struct Geometry
    {
      RenderGraphResource vertices, indices, uniform, parameters;
      RenderMeshHandle mesh;
      entt::entity entity;
      DescriptorSetHandle set;
      std::shared_ptr<const MaterialInstance> instance;
    };
    struct Group
    {
      std::shared_ptr<const ShadingModel> model;
      std::vector<Geometry> geometry;
      std::vector<RenderGraphResource> terrain;
      DescriptorSetHandle terrainSet{};
    };
    std::map<SurfaceModel, Group> groups;
    auto import = [&](const char* name, BufferHandle buffer)
    {
      const auto desc = device.GetBufferDesc(buffer);
      return context.graph.Import(name,
                                  ImportedRenderGraphBuffer::Desc{
                                      .size = desc->size, .usage = desc->usage},
                                  ImportedRenderGraphBuffer{buffer});
    };
    auto makeFrame =
        [&](const std::shared_ptr<const MaterialInstance>& instance,
            const MaterialFrameData& frame)
    {
      m_draws.emplace_back();
      auto& draw = m_draws.back();
      draw.uniform = device.CreateBuffer(
          {.size = sizeof(frame),
           .usage = BufferUsage::Uniform,
           .initialData = std::as_bytes(std::span(&frame, 1))});
      draw.set = device.CreateDescriptorSet(
          {.layout = instance->material->frameLayout});
      const std::array writes{
          DescriptorWrite{.binding = 0,
                          .resource = DescriptorBufferInfo{
                              .buffer = draw.uniform, .range = sizeof(frame)}}};
      device.UpdateDescriptorSet(draw.set, writes);
      return draw;
    };
    auto entities = m_world->Registry().view<Transform, MeshRender>();
    for (auto entity : entities)
    {
      const auto& component = entities.get<MeshRender>(entity);
      const auto* mesh = m_renderer->GetRenderMesh(component.mesh);
      if (!mesh || m_renderer->GetRenderMeshUploadStatus(component.mesh) !=
                       RenderMeshUploadStatus::Ready)
        continue;
      if (!component.material)
        throw std::logic_error(
            "WorldRenderer requires a MaterialInstance for each mesh");
      if (mesh->layout.bindings.size() != 1 ||
          mesh->vertexBuffers.size() != 1 || !mesh->indexBuffer ||
          mesh->indexBuffer->format != IndexFormat::UInt32 ||
          mesh->topology != PrimitiveTopology::TriangleList ||
          mesh->layout.bindings.front().stride != sizeof(Mesh3DVertex))
        throw std::logic_error(
            "WorldRenderer expects indexed Mesh3DVertex geometry");
      const auto instance = component.material;
      const auto draw = makeFrame(
          instance, {.viewProjection = ViewProjection(),
                     .model = entities.get<Transform>(entity).GetMatrix()});
      auto& group = groups[instance->material->model];
      group.model = instance->material->shadingModel;
      group.geometry.push_back(
          {import("World vertices", mesh->vertexBuffers.front().buffer),
           import("World indices", mesh->indexBuffer->buffer),
           import("Object frame", draw.uniform),
           import("Material parameters", instance->parametersBuffer),
           component.mesh, entity, draw.set, instance});
    }
    m_terrainPool.Rebuild(m_world->GetTerrain());
    if (m_terrainPool.Vertices())
    {
      const auto draw =
          makeFrame(m_terrainMaterial, {.viewProjection = ViewProjection()});
      auto& group = groups[m_terrainMaterial->material->model];
      group.model = m_terrainMaterial->material->shadingModel;
      group.terrainSet = draw.set;
      for (auto buffer : {m_terrainPool.Vertices(), m_terrainPool.Indices(),
                          m_terrainPool.Indirect(), draw.uniform,
                          m_terrainMaterial->parametersBuffer})
        group.terrain.push_back(import("Terrain buffer", buffer));
    }
    // An empty world still clears a valid output through the Unlit passes.
    if (groups.empty())
      groups[SurfaceModel::Unlit].model =
          m_terrainMaterial->material->shadingModel;
    RenderGraphResource sceneLights = kInvalidGraphResource;
    RenderGraphResource lightVertices = kInvalidGraphResource,
                        lightIndices = kInvalidGraphResource;
    RenderGraphResource lightDraws = kInvalidGraphResource;
    if (groups.contains(SurfaceModel::Lit))
    {
      const auto lights = GatherLights();
      m_lightCapacity = std::max(size_t{1}, lights.size());
      const auto bytes = LightingBytes(lights);
      m_lightBuffer = device.CreateBuffer({.size = bytes.size(),
                                           .usage = BufferUsage::Storage,
                                           .initialData = bytes});
      sceneLights = import("Scene lights", m_lightBuffer);
      const auto draws = VolumeCommands(lights);
      m_lightIndirect =
          device.CreateBuffer({.size = sizeof(draws),
                               .usage = BufferUsage::Indirect,
                               .initialData = std::as_bytes(std::span(draws))});
      const auto* volume = m_renderer->GetRenderMesh(m_volumes[0]);
      lightVertices =
          import("Light volume vertices", volume->vertexBuffers.front().buffer);
      lightIndices =
          import("Light volume indices", volume->indexBuffer->buffer);
      lightDraws = import("Instanced light volume draws", m_lightIndirect);
    }
    struct GBufferData
    {
      std::vector<RenderGraphResource> attributes;
      RenderGraphResource depth;
      Group group;
    };
    struct LightingData
    {
      std::vector<RenderGraphResource> attributes;
      RenderGraphResource depth, color;
      RenderGraphResource lights = kInvalidGraphResource;
      RenderGraphResource vertices = kInvalidGraphResource,
                          indices = kInvalidGraphResource;
      RenderGraphResource draws = kInvalidGraphResource;
    };
    struct PendingLighting
    {
      std::shared_ptr<const ShadingModel> model;
      std::vector<RenderGraphResource> attributes;
      uint32_t stencil;
    };
    std::vector<PendingLighting> lighting;
    // Allocate one image per unique attribute, regardless of how many models
    // use it.
    std::map<GBufferAttribute, RenderGraphResource> sharedAttributes;
    for (const auto& [_, group] : groups)
      for (auto attribute : group.model->attributes)
        sharedAttributes.emplace(attribute, kInvalidGraphResource);
    struct ClearData
    {
      std::map<GBufferAttribute, RenderGraphResource> attributes;
      RenderGraphResource depth;
    };
    const auto& clear = context.graph.AddPass<ClearData>(
        "Create GBuffer Attributes",
        [size, sharedAttributes](RenderGraph::Builder& builder, ClearData& data)
        {
          for (const auto& [attribute, _] : sharedAttributes)
          {
            const auto& declaration =
                GBufferAttributes[static_cast<size_t>(attribute)];
            const auto image = builder.Create<RenderGraphImage>(
                declaration.name,
                {.width = size.x,
                 .height = size.y,
                 .format = declaration.format,
                 .usage = {ImageUsage::ColorAttachment, ImageUsage::Sampled}});
            data.attributes.emplace(attribute, builder.Write(image));
          }
          data.depth = builder.Create<RenderGraphImage>(
              "Material IDs (depth/stencil)",
              {.width = size.x,
               .height = size.y,
               .format = ImageFormat::Depth24Stencil8,
               .usage = {ImageUsage::DepthStencilAttachment}});
          data.depth = builder.Write(data.depth);
        },
        [size](const ClearData& data, FrameGraphPassResources& resources,
               RenderGraphExecutePassContext& execute)
        {
          RenderPassDesc rendering;
          rendering.depthAttachment =
              resources.get<RenderGraphImage>(data.depth).viewHandle;
          rendering.clearColor = true;
          rendering.clearDepth = true;
          rendering.width = size.x;
          rendering.height = size.y;
          for (const auto& [_, image] : data.attributes)
            rendering.colorAttachments.push_back(
                resources.get<RenderGraphImage>(image).viewHandle);
          execute.commandList->Begin();
          execute.commandList->BeginRendering(rendering);
          execute.commandList->EndRendering();
          execute.commandList->End();
        });
    sharedAttributes = clear.attributes;
    RenderGraphResource depth = clear.depth;
    uint32_t id = 0;
    for (const auto& [_, group] : groups)
    {
      const uint32_t stencil = ++id;
      const auto& pass = context.graph.AddPass<GBufferData>(
          std::string(boost::describe::enum_to_string(group.model->model,"<INVALID>")) + " G-buffer fill",
          [group, depth, sharedAttributes](RenderGraph::Builder& builder,
                                           GBufferData& data)
          {
            data.group = group;
            for (const auto attribute : group.model->attributes)
            {
              data.attributes.push_back(
                  builder.Write(builder.Read(sharedAttributes.at(attribute))));
            }
            data.depth = builder.Read(depth);
            data.depth = builder.Write(data.depth);
            for (auto& draw : data.group.geometry)
            {
              draw.vertices = builder.Read(draw.vertices);
              draw.indices = builder.Read(draw.indices);
              draw.uniform = builder.Read(draw.uniform);
              draw.parameters = builder.Read(draw.parameters);
            }
            for (auto& resource : data.group.terrain)
              resource = builder.Read(resource);
          },
          [this, size, stencil](const GBufferData& data,
                                FrameGraphPassResources& resources,
                                RenderGraphExecutePassContext& execute)
          {
            auto& commands = *execute.commandList;
            RenderPassDesc rendering;
            rendering.depthAttachment =
                resources.get<RenderGraphImage>(data.depth).viewHandle;
            SetViewport(rendering, size);
            for (auto resource : data.attributes)
              rendering.colorAttachments.push_back(
                  resources.get<RenderGraphImage>(resource).viewHandle);
            commands.Begin();
            commands.BeginRendering(rendering);
            commands.SetStencil(StencilMode::Write, stencil);
            for (const auto& draw : data.group.geometry)
            {
              auto& registry = m_world->Registry();
              if (!registry.valid(draw.entity) ||
                  !registry.all_of<Transform, MeshRender>(draw.entity) ||
                  registry.get<MeshRender>(draw.entity).material !=
                      draw.instance)
                continue;
              const auto* mesh = m_renderer->GetRenderMesh(draw.mesh);
              if (!mesh)
                continue;
              const MaterialFrameData frame{
                  .viewProjection = ViewProjection(),
                  .model = registry.get<Transform>(draw.entity).GetMatrix()};
              m_renderer->GetRenderDevice()->UpdateBuffer(
                  resources.get<ImportedRenderGraphBuffer>(draw.uniform).handle,
                  0, std::as_bytes(std::span(&frame, 1)));
              commands.BindPipeline(draw.instance->material->gBufferPipeline);
              commands.BindDescriptorSet(0, draw.set);
              commands.BindDescriptorSet(1, draw.instance->parametersSet);
              commands.BindVertexBuffer(
                  resources.get<ImportedRenderGraphBuffer>(draw.vertices)
                      .handle,
                  mesh->layout.bindings.front().stride);
              commands.BindIndexBuffer(
                  resources.get<ImportedRenderGraphBuffer>(draw.indices)
                      .handle);
              commands.DrawIndexedRange(
                  mesh->indexCount,
                  static_cast<uint32_t>(mesh->indexBuffer->offset /
                                        sizeof(uint32_t)),
                  static_cast<int32_t>(mesh->vertexBuffers.front().offset /
                                       mesh->layout.bindings.front().stride));
            }
            if (data.group.terrainSet)
            {
              const MaterialFrameData frame{.viewProjection = ViewProjection()};
              m_renderer->GetRenderDevice()->UpdateBuffer(
                  resources
                      .get<ImportedRenderGraphBuffer>(data.group.terrain[3])
                      .handle,
                  0, std::as_bytes(std::span(&frame, 1)));
              commands.BindPipeline(
                  m_terrainMaterial->material->gBufferPipeline);
              commands.BindDescriptorSet(0, data.group.terrainSet);
              commands.BindDescriptorSet(1, m_terrainMaterial->parametersSet);
              m_terrainPool.Draw(commands);
            }
            commands.EndRendering();
            commands.End();
          });
      depth = pass.depth;
      for (size_t i = 0; i < group.model->attributes.size(); ++i)
        sharedAttributes[group.model->attributes[i]] = pass.attributes[i];
      lighting.push_back({group.model, pass.attributes, stencil});
    }
    RenderGraphResource output = kInvalidGraphResource;
    for (auto& pass : lighting)
    {
      pass.attributes.clear();
      for (auto attribute : pass.model->attributes)
        pass.attributes.push_back(sharedAttributes.at(attribute));
      m_lighting.emplace_back();
      auto& retained = m_lighting.back();
      retained.model = pass.model;
      retained.set = device.CreateDescriptorSet(
          {.layout = pass.model->deferredLightingPipeline.attributesLayout});
      if (pass.model->deferredLightingPipeline.lightingLayout)
        retained.lights = device.CreateDescriptorSet(
            {.layout = pass.model->deferredLightingPipeline.lightingLayout});
      const auto lightSet = retained.lights;
      const auto set = retained.set;
      const bool first = output == kInvalidGraphResource;
      const auto& result = context.graph.AddPass<LightingData>(
        std::string(boost::describe::enum_to_string(pass.model->model,"<INVALID>")) + " Deferred lighting",
          [size, pass, depth, output, first, sceneLights, lightVertices,
           lightIndices,
           lightDraws](RenderGraph::Builder& builder, LightingData& data)
          {
            for (auto resource : pass.attributes)
              data.attributes.push_back(builder.Read(resource));
            data.depth = builder.Read(depth);
            if (pass.model->deferredLightingPipeline.lightingLayout)
            {
              data.lights = builder.Read(sceneLights);
              data.vertices = builder.Read(lightVertices);
              data.indices = builder.Read(lightIndices);
              data.draws = builder.Read(lightDraws);
            }
            if (first)
              data.color = builder.Create<RenderGraphImage>(
                  "Linear lighting accumulation",
                  {.width = size.x,
                   .height = size.y,
                   .format = ImageFormat::RGBA16f,
                   .usage = {ImageUsage::ColorAttachment,
                             ImageUsage::TransferSource, ImageUsage::Sampled}});
            else
              data.color = builder.Read(output);
            data.color = builder.Write(data.color);
          },
          [this, size, pass, set, lightSet,
           first](const LightingData& data, FrameGraphPassResources& resources,
                  RenderGraphExecutePassContext& execute)
          {
            auto& device = *m_renderer->GetRenderDevice();
            std::vector<DescriptorWrite> writes;
            for (size_t i = 0; i < data.attributes.size(); ++i)
              writes.push_back(
                  {.binding = static_cast<uint32_t>(pass.model->attributes[i]),
                   .resource = DescriptorImageInfo{
                       .image =
                           resources.get<RenderGraphImage>(data.attributes[i])
                               .viewHandle,
                       .sampler = m_sampler}});
            device.UpdateDescriptorSet(set, writes);
            if (lightSet)
            {
              const auto lights = GatherLights();
              if (lights.size() > m_lightCapacity)
                throw std::logic_error("Scene light capacity changed; call "
                                       "Renderer::Compile after adding lights");
              const auto bytes = LightingBytes(lights);
              const auto buffer =
                  resources.get<ImportedRenderGraphBuffer>(data.lights).handle;
              device.UpdateBuffer(buffer, 0, bytes);
              const auto draws = VolumeCommands(lights);
              device.UpdateBuffer(
                  resources.get<ImportedRenderGraphBuffer>(data.draws).handle,
                  0, std::as_bytes(std::span(draws)));
              const std::array lightWrites{DescriptorWrite{
                  .binding = 0,
                  .resource = DescriptorBufferInfo{.buffer = buffer,
                                                   .range = bytes.size()}}};
              device.UpdateDescriptorSet(lightSet, lightWrites);
            }
            RenderPassDesc rendering;
            rendering.colorAttachment =
                resources.get<RenderGraphImage>(data.color).viewHandle;
            rendering.depthAttachment =
                resources.get<RenderGraphImage>(data.depth).viewHandle;
            rendering.clearColor = first;
            rendering.clearColorAlpha = 0.0f;
            rendering.depthReadOnly = true;
            SetViewport(rendering, size);
            auto& commands = *execute.commandList;
            commands.Begin();
            commands.BeginRendering(rendering);
            commands.SetStencil(StencilMode::Equal, pass.stencil);
            commands.BindPipeline(
                pass.model->deferredLightingPipeline.pipeline);
            commands.BindDescriptorSet(0, set);
            if (lightSet)
            {
              commands.BindDescriptorSet(1, lightSet);
              commands.BindVertexBuffer(
                  resources.get<ImportedRenderGraphBuffer>(data.vertices)
                      .handle,
                  sizeof(Mesh3DVertex));
              commands.BindIndexBuffer(
                  resources.get<ImportedRenderGraphBuffer>(data.indices)
                      .handle);
              commands.MultiDrawIndexedIndirect(
                  resources.get<ImportedRenderGraphBuffer>(data.draws).handle,
                  3);
            }
            else
              commands.Draw(PrimitiveTopology::TriangleList, 3, 0);
            commands.EndRendering();
            commands.End();
          });
      output = result.color;
    }
    struct OutputData
    {
      RenderGraphResource input, color;
    };
    m_outputSet = device.CreateDescriptorSet({.layout = m_outputSetLayout});
    const auto outputSet = m_outputSet;
    const auto& encoded = context.graph.AddPass<OutputData>(
        "Encode lighting output",
        [size, output](RenderGraph::Builder& builder, OutputData& data)
        {
          data.input = builder.Read(output);
          data.color = builder.Create<RenderGraphImage>(
              "World color",
              {.width = size.x,
               .height = size.y,
               .format = ImageFormat::RGBA8,
               .usage = {ImageUsage::ColorAttachment,
                         ImageUsage::TransferSource, ImageUsage::Sampled}});
          data.color = builder.Write(data.color);
        },
        [this, size, outputSet](const OutputData& data,
                                FrameGraphPassResources& resources,
                                RenderGraphExecutePassContext& execute)
        {
          auto& device = *m_renderer->GetRenderDevice();
          const std::array writes{DescriptorWrite{
              .binding = 0,
              .resource = DescriptorImageInfo{
                  .image =
                      resources.get<RenderGraphImage>(data.input).viewHandle,
                  .sampler = m_sampler}}};
          device.UpdateDescriptorSet(outputSet, writes);
          auto& commands = *execute.commandList;
          commands.Begin();
          commands.BeginRendering(
              {.colorAttachment =
                   resources.get<RenderGraphImage>(data.color).viewHandle,
               .width = size.x,
               .height = size.y});
          commands.BindPipeline(m_outputPipeline);
          commands.BindDescriptorSet(0, outputSet);
          commands.Draw(PrimitiveTopology::TriangleList, 3, 0);
          commands.EndRendering();
          commands.End();
        });
    m_output = encoded.color;
  }
  void SetAmbientRadiance(Vec3 radiance)
  {
    if (!Finite(radiance) || glm::any(glm::lessThan(radiance, Vec3(0.0f))))
      throw std::invalid_argument(
          "Ambient radiance must be finite and nonnegative");
    m_ambientRadiance = radiance;
  }
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
  void EnsureLightVolumes()
  {
    if (std::all_of(m_volumes.begin(), m_volumes.end(),
                    [](auto mesh) { return bool(mesh); }))
      return;
    const auto meshes = Detail::MakeLightVolumes();
    for (size_t i = 0; i < meshes.size(); ++i)
      if (!m_volumes[i])
        m_volumes[i] = m_renderer->UploadMesh(meshes[i]);
  }
  std::array<Lighthouse::DrawIndexedIndirectCommand, 3>
  VolumeCommands(const std::vector<LightData>& lights) const
  {
    using namespace Lighthouse;
    std::array<DrawIndexedIndirectCommand, 3> draws{};
    draws[0].instanceCount = 1; // Ambient/emission base instance.
    for (const auto& light : lights)
      ++draws[static_cast<size_t>(light.directionType.w)].instanceCount;
    for (size_t shape = 0; shape < draws.size(); ++shape)
    {
      const auto* mesh = m_renderer->GetRenderMesh(m_volumes[shape]);
      draws[shape].indexCount = mesh->indexCount;
      draws[shape].firstIndex =
          static_cast<uint32_t>(mesh->indexBuffer->offset / sizeof(uint32_t));
      draws[shape].baseVertex = static_cast<int32_t>(
          mesh->vertexBuffers.front().offset / sizeof(Mesh3DVertex));
      draws[shape].firstInstance = 0;
    }
    return draws;
  }
  void CreateOutputPipeline(Lighthouse::IRenderDevice& device)
  {
    using namespace Lighthouse;
    m_outputSetLayout = device.CreateDescriptorSetLayout(
        {.bindings = {{.binding = 0,
                       .type = DescriptorType::CombinedImageSampler,
                       .count = 1,
                       .stages = ShaderStageFlags::Fragment}}});
    m_outputLayout = device.CreatePipelineLayout({.sets = {m_outputSetLayout}});
    m_outputVertex = device.CreateShaderModule(
        ShaderCompiler::Compile(R"glsl(#version 450
void main() { vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3)); gl_Position=vec4(p[gl_VertexIndex],0,1); }
)glsl",
                                {.stage = ShaderStage::Vertex}));
    m_outputFragment = device.CreateShaderModule(
        ShaderCompiler::Compile(R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D lighting;
layout(location=0) out vec4 color;
void main() {
    vec4 value=texelFetch(lighting,ivec2(gl_FragCoord.xy),0);
    vec3 linear=max(value.rgb,vec3(0));
    vec3 srgb=mix(linear*12.92,1.055*pow(linear,vec3(1.0/2.4))-0.055,
                  greaterThan(linear,vec3(0.0031308)));
    color=value.a > 0.0 ? vec4(srgb,value.a) : vec4(0.025,0.035,0.05,1.0);
}
)glsl",
                                {.stage = ShaderStage::Fragment}));
    m_outputPipeline =
        device.CreatePipeline({.vertexShader = m_outputVertex,
                               .fragmentShader = m_outputFragment,
                               .colorFormats = {ImageFormat::RGBA8},
                               .layout = m_outputLayout});
  }
  static bool Finite(Vec3 value)
  {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
  }
  std::vector<LightData> GatherLights() const
  {
    std::vector<LightData> result;
    auto view = m_world->Registry().view<Transform, Light>();
    for (auto entity : view)
    {
      const auto& light = view.get<Light>(entity);
      if (!light.enabled)
        continue;
      const auto& transform = view.get<Transform>(entity);
      Vec3 direction = transform.GetRot() * Vec3(0, 0, -1);
      const bool local = light.type != LightType::Directional;
      if (static_cast<uint32_t>(light.type) >
              static_cast<uint32_t>(LightType::Spot) ||
          !Finite(light.color) ||
          glm::any(glm::lessThan(light.color, Vec3(0))) ||
          !std::isfinite(light.intensity) || light.intensity < 0 ||
          !Finite(transform.GetPos()) || !Finite(direction) ||
          glm::dot(direction, direction) < 1e-8f ||
          (local && (!std::isfinite(light.range) || light.range <= 0)) ||
          (light.type == LightType::Spot &&
           (!std::isfinite(light.innerAngle) ||
            !std::isfinite(light.outerAngle) || light.innerAngle < 0 ||
            light.outerAngle < light.innerAngle || light.outerAngle <= 0 ||
            light.outerAngle >= 90)))
        throw std::invalid_argument(
            "Invalid light: check type, color, intensity, transform, range and "
            "spot half angles");
      direction = glm::normalize(direction);
      result.push_back(
          {.colorIntensity = Vec4(light.color, light.intensity),
           .positionRange = Vec4(transform.GetPos(), light.range),
           .directionType = Vec4(direction, static_cast<float>(light.type)),
           .spotCosines =
               light.type == LightType::Spot
                   ? Vec4(std::cos(glm::radians(light.innerAngle)),
                          std::cos(glm::radians(light.outerAngle)), 0, 0)
                   : Vec4(0)});
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const auto& a, const auto& b)
                     { return a.directionType.w < b.directionType.w; });
    if (result.size() >= std::numeric_limits<uint32_t>::max())
      throw std::length_error("Too many scene lights");
    return result;
  }
  std::vector<std::byte>
  LightingBytes(const std::vector<LightData>& lights) const
  {
    std::vector<std::byte> bytes(sizeof(LightingFrameData) +
                                 (m_lightCapacity + 1) * sizeof(LightData));
    LightingFrameData frame;
    frame.ambientRadiance = Vec4(m_ambientRadiance, 0);
    frame.viewProjection = ViewProjection();
    frame.lightCount = {static_cast<uint32_t>(lights.size()) + 1, 0, 1, 1};
    for (const auto& light : lights)
    {
      if (light.directionType.w == 0)
      {
        ++frame.lightCount[2];
        ++frame.lightCount[3];
      }
      else if (light.directionType.w == 1)
        ++frame.lightCount[3];
    }
    std::memcpy(bytes.data(), &frame, sizeof(frame));
    LightData base;
    std::memcpy(bytes.data() + sizeof(frame), &base, sizeof(base));
    if (!lights.empty())
      std::memcpy(bytes.data() + sizeof(frame) + sizeof(base), lights.data(),
                  lights.size() * sizeof(LightData));
    return bytes;
  }
  static void SetViewport(Lighthouse::RenderPassDesc& pass, UVec2 size)
  {
    pass.width = std::min(size.x, size.y * 16u / 9u);
    pass.height = std::min(size.y, size.x * 9u / 16u);
    pass.offsetX = static_cast<int32_t>((size.x - pass.width) / 2);
    pass.offsetY = static_cast<int32_t>((size.y - pass.height) / 2);
  }
  static Mat4 ViewProjection()
  {
    const Mat4 view(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -3.6f, 1);
    const Mat4 projection(1.357995f, 0, 0, 0, 0, 2.414214f, 0, 0, 0, 0,
                          -1.001001f, -1, 0, 0, -0.1001001f, 0);
    return projection * view;
  }
  void ReleaseDraws()
  {
    auto& device = *m_renderer->GetRenderDevice();
    for (const auto& draw : m_draws)
    {
      if (draw.set)
        device.DestroyDescriptorSet(draw.set);
      if (draw.uniform)
        device.DestroyBuffer(draw.uniform);
    }
    m_draws.clear();
    for (const auto& lighting : m_lighting)
    {
      if (lighting.set)
        device.DestroyDescriptorSet(lighting.set);
      if (lighting.lights)
        device.DestroyDescriptorSet(lighting.lights);
    }
    m_lighting.clear();
    if (m_lightBuffer)
      device.DestroyBuffer(m_lightBuffer);
    if (m_lightIndirect)
      device.DestroyBuffer(m_lightIndirect);
    m_lightIndirect = {};
    if (m_outputSet)
      device.DestroyDescriptorSet(m_outputSet);
    m_outputSet = {};
    m_lightBuffer = {};
    m_lightCapacity = 0;
  }
  struct DrawResources
  {
    Lighthouse::BufferHandle uniform{};
    Lighthouse::DescriptorSetHandle set{};
  };
  struct LightingResources
  {
    std::shared_ptr<const ShadingModel> model;
    Lighthouse::DescriptorSetHandle set{}, lights{};
  };
  Lighthouse::Renderer* m_renderer;
  World* m_world;
  UVec2 m_resolution;
  Lighthouse::RenderGraphResource m_output{Lighthouse::kInvalidGraphResource};
  std::vector<DrawResources> m_draws;
  std::vector<LightingResources> m_lighting;
  Lighthouse::SamplerHandle m_sampler{};
  Lighthouse::BufferHandle m_lightBuffer{}, m_lightIndirect{};
  std::array<Lighthouse::RenderMeshHandle, 3> m_volumes{};
  Lighthouse::DescriptorSetLayoutHandle m_outputSetLayout{};
  Lighthouse::PipelineLayoutHandle m_outputLayout{};
  Lighthouse::GraphicsPipelineHandle m_outputPipeline{};
  Lighthouse::ShaderModuleHandle m_outputVertex{}, m_outputFragment{};
  Lighthouse::DescriptorSetHandle m_outputSet{};
  size_t m_lightCapacity = 0;
  Vec3 m_ambientRadiance{0.03f};
  TerrainMeshPool m_terrainPool;
  Materials m_materials;
  std::shared_ptr<const MaterialInstance> m_terrainMaterial;
};
} // namespace EESG
