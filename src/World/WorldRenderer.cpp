#include "WorldRenderer.hpp"
#include "HostBuffer.hpp"

#include "World/Entity/Light.hpp"
#include "World/Entity/Transform.hpp"
#include "World/LightVolumes.hpp"
#include <Lighthouse/Engine/Assets/Mesh.hpp>
#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <algorithm>
#include <array>
#include <boost/describe/enum_to_string.hpp>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <map>

namespace EESG
{
WorldRenderer::WorldRenderer(Lighthouse::Renderer* renderer, World* world,
                             Lighthouse::RenderViewHandle window)
    : m_renderer(renderer), m_world(world), m_window(window),
      m_materials(*renderer->GetRenderDevice()), m_entities(*renderer, *world)
{
}

WorldRenderer::~WorldRenderer()
{
  auto& device = *m_renderer->GetRenderDevice();
  device.WaitIdle();
  ReleaseDraws();
  m_world->GetTerrain().Release();
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

void WorldRenderer::BuildGraph(Lighthouse::TechniqueBuildContext& context)
{
  using namespace Lighthouse;
  auto& device = context.device;
  device.WaitIdle();
  ReleaseDraws();
  if (!m_sampler)
    m_sampler = device.CreateSampler({});
  EnsureLightVolumes();
  if (!m_outputPipeline)
    CreateOutputPipeline(device);
  m_world->GetTerrain().BuildGraph(context.graph, device,
                                   m_world->GetTerrainObservers());
  m_output = kInvalidGraphResource;
  auto cameras = m_world->Registry().view<Player, Transform>();
  for (auto entity : cameras)
  {
    auto& camera = cameras.get<Player>(entity).cameraView;
    camera.m_output = kInvalidGraphResource;
    BuildCamera(context, entity, camera);
    if (m_output == kInvalidGraphResource)
      m_output = camera.GetOutput();
  }
  auto* windowView = context.renderer.GetView<WindowView>(m_window);
  if (windowView && m_output != kInvalidGraphResource)
  {
    struct PresentData
    {
      RenderGraphResource source, destination;
    };
    context.graph.AddPass<PresentData>(
        "Present world",
        [this, windowView](RenderGraph::Builder& builder, PresentData& data)
        {
          data.source = builder.Read(m_output);
          data.destination =
              builder.Write(windowView->GetAcquiredImageAsRGResource());
        },
        [windowView](const PresentData& data,
                     FrameGraphPassResources& resources,
                     RenderGraphExecutePassContext& execute)
        {
          const auto& source = resources.get<RenderGraphImage>(data.source);
          const auto& description =
              resources.getDescriptor<RenderGraphImage>(data.source);
          const auto& destination =
              resources.get<ImportedWindowView>(data.destination);
          const auto size = windowView->GetWindow()->GetFramebufferSize();
          const auto width = std::min(
              size.width, size.height * description.width / description.height);
          const auto height = std::min(
              size.height, size.width * description.height / description.width);
          const auto x = (size.width - width) / 2;
          const auto y = (size.height - height) / 2;
          const BlitRegion region{
              .srcSubresource = {ImageAspect::Color, 0, 0, 1},
              .srcOffsets = {{{0, 0, 0},
                              {description.width, description.height, 1}}},
              .dstSubresource = {ImageAspect::Color, 0, 0, 1},
              .dstOffsets = {{{x, y, 0}, {x + width, y + height, 1}}}};
          execute.commandList->Begin();
          execute.commandList->BeginRendering(
              {.colorAttachment = destination.currentImageView,
               .clearColor = true,
               .clearColorValue = {0.025f, 0.035f, 0.05f},
               .width = size.width,
               .height = size.height});
          execute.commandList->EndRendering();
          execute.commandList->BlitImage(source.handle,
                                         destination.currentImage, region,
                                         ImageFilter::Linear);
          execute.commandList->End();
        });
  }
}

void WorldRenderer::BuildCamera(Lighthouse::TechniqueBuildContext& context,
                                entt::entity entity, CameraView& camera)
{
  using namespace Lighthouse;
  auto& device = context.device;
  const auto size = camera.GetResolution();
  const auto projection = [this, entity] { return ViewProjection(entity); };
  BufferHandle lightBuffer{}, lightIndirect{};
  size_t lightCapacity = 0;
  GBufferResources gbuffer;
  auto process = [&](PostProcessStage stage, RenderGraphResource color)
  {
    for (const auto& effect : camera.postProcesses)
    {
      if (!effect || effect->Stage() != stage)
        continue;
      const auto* view = context.renderer.GetView<WindowView>(m_window);
      if (!view)
        throw std::logic_error("Post-processing requires an output RenderView");
      const PostProcessContext post{device, *view,  gbuffer,
                                    size.x, size.y, m_deltaTime};
      color = effect->BuildGraph(context, post, color);
    }
    return color;
  };
  PreparedGeometry groups;
  auto import = [&](const char* name, BufferHandle buffer)
  {
    const auto desc = device.GetBufferDesc(buffer);
    return context.graph.Import(name,
                                ImportedRenderGraphBuffer::Desc{
                                    .size = desc->size, .usage = desc->usage},
                                ImportedRenderGraphBuffer{buffer});
  };
  m_entities.Prepare(context, projection(), groups);
  RenderGraphResource sceneLights = kInvalidGraphResource;
  RenderGraphResource lightVertices = kInvalidGraphResource,
                      lightIndices = kInvalidGraphResource;
  RenderGraphResource lightDraws = kInvalidGraphResource;
  if (groups.contains(SurfaceModel::Lit))
  {
    const auto lights = GatherLights();
    lightCapacity = std::max(size_t{1}, lights.size());
    const auto bytes = LightingBytes(lights, lightCapacity, projection());
    lightBuffer = device.CreateBuffer(
        {.size = bytes.size(),
         .usage = BufferUsage::Storage,
         .initialData = bytes,
         .memory = {.requiredAccess =
                        MemoryAccess::CPUWrite | MemoryAccess::GPURead}});
    m_lightBuffers.push_back(lightBuffer);
    sceneLights = import("Scene lights", lightBuffer);
    const auto draws = VolumeCommands(lights);
    lightIndirect = device.CreateBuffer(
        {.size = sizeof(draws),
         .usage = BufferUsage::Indirect,
         .initialData = std::as_bytes(std::span(draws)),
         .memory = {.requiredAccess =
                        MemoryAccess::CPUWrite | MemoryAccess::GPURead}});
    m_lightBuffers.push_back(lightIndirect);
    const auto* volume = m_renderer->GetRenderMesh(m_volumes[0]);
    lightVertices =
        import("Light volume vertices", volume->vertexBuffers.front().buffer);
    lightIndices = import("Light volume indices", volume->indexBuffer->buffer);
    lightDraws = import("Instanced light volume draws", lightIndirect);
  }
  struct GBufferData
  {
    std::vector<RenderGraphResource> attributes;
    RenderGraphResource depth;
    GeometryGroup group;
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
        std::string(
            boost::describe::enum_to_string(group.model->model, "<INVALID>")) +
            " G-buffer fill",
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
        },
        [this, size, stencil, projection](
            const GBufferData& data, FrameGraphPassResources& resources,
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
          m_entities.Draw(data.group, projection(), resources, execute);
          commands.EndRendering();
          commands.End();
        });
    depth = pass.depth;
    for (size_t i = 0; i < group.model->attributes.size(); ++i)
      sharedAttributes[group.model->attributes[i]] = pass.attributes[i];
    lighting.push_back({group.model, pass.attributes, stencil});
  }
  gbuffer = {sharedAttributes, depth};
  process(PostProcessStage::AfterGBuffer, kInvalidGraphResource);
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
        std::string(
            boost::describe::enum_to_string(pass.model->model, "<INVALID>")) +
            " Deferred lighting",
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
        [this, size, pass, set, lightSet, first, lightCapacity, projection](
            const LightingData& data, FrameGraphPassResources& resources,
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
            if (lights.size() > lightCapacity)
              throw std::logic_error("Scene light capacity changed; call "
                                     "Renderer::Compile after adding lights");
            const auto bytes =
                LightingBytes(lights, lightCapacity, projection());
            const auto buffer =
                resources.get<ImportedRenderGraphBuffer>(data.lights).handle;
            WriteHostBuffer(device, buffer, bytes);
            const auto draws = VolumeCommands(lights);
            WriteHostBuffer(
                device,
                resources.get<ImportedRenderGraphBuffer>(data.draws).handle,
                std::as_bytes(std::span(draws)));
            const std::array lightWrites{
                DescriptorWrite{.binding = 0,
                                .resource = DescriptorBufferInfo{
                                    .buffer = buffer, .range = bytes.size()}}};
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
          commands.BindPipeline(pass.model->deferredLightingPipeline.pipeline);
          commands.BindDescriptorSet(0, set);
          if (lightSet)
          {
            commands.BindDescriptorSet(1, lightSet);
            commands.BindVertexBuffer(
                resources.get<ImportedRenderGraphBuffer>(data.vertices).handle,
                sizeof(Mesh3DVertex));
            commands.BindIndexBuffer(
                resources.get<ImportedRenderGraphBuffer>(data.indices).handle);
            commands.MultiDrawIndexedIndirect(
                resources.get<ImportedRenderGraphBuffer>(data.draws).handle, 3);
          }
          else
            commands.Draw(PrimitiveTopology::TriangleList, 3, 0);
          commands.EndRendering();
          commands.End();
        });
    output = result.color;
  }
  output = process(PostProcessStage::AfterLighting, output);
  output = process(PostProcessStage::BeforeToneMapping, output);
  struct OutputData
  {
    RenderGraphResource input, color;
  };
  const auto outputSet =
      device.CreateDescriptorSet({.layout = m_outputSetLayout});
  m_outputSets.push_back(outputSet);
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
             .usage = {ImageUsage::ColorAttachment, ImageUsage::TransferSource,
                       ImageUsage::Sampled}});
        data.color = builder.Write(data.color);
        // Camera outputs remain available even when not selected for
        // presentation.
        builder.SetSideEffect();
      },
      [this, size, outputSet](const OutputData& data,
                              FrameGraphPassResources& resources,
                              RenderGraphExecutePassContext& execute)
      {
        auto& device = *m_renderer->GetRenderDevice();
        const std::array writes{DescriptorWrite{
            .binding = 0,
            .resource = DescriptorImageInfo{
                .image = resources.get<RenderGraphImage>(data.input).viewHandle,
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
  camera.m_output = process(PostProcessStage::AfterToneMapping, encoded.color);
  camera.m_output =
      process(PostProcessStage::BeforePresentation, camera.m_output);
}

void WorldRenderer::SetAmbientRadiance(Vec3 radiance)
{
  if (!Finite(radiance) || glm::any(glm::lessThan(radiance, Vec3(0.0f))))
    throw std::invalid_argument(
        "Ambient radiance must be finite and nonnegative");
  m_ambientRadiance = radiance;
}

Lighthouse::RenderGraphResource WorldRenderer::GetOutput() const noexcept
{
  return m_output;
}

void WorldRenderer::EnsureLightVolumes()
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
WorldRenderer::VolumeCommands(const std::vector<LightData>& lights) const
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

void WorldRenderer::CreateOutputPipeline(Lighthouse::IRenderDevice& device)
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

bool WorldRenderer::Finite(Vec3 value)
{
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

std::vector<LightData> WorldRenderer::GatherLights() const
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
        !Finite(light.color) || glm::any(glm::lessThan(light.color, Vec3(0))) ||
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
WorldRenderer::LightingBytes(const std::vector<LightData>& lights,
                             size_t lightCapacity,
                             const Mat4& viewProjection) const
{
  std::vector<std::byte> bytes(sizeof(LightingFrameData) +
                               (lightCapacity + 1) * sizeof(LightData));
  LightingFrameData frame;
  frame.ambientRadiance = Vec4(m_ambientRadiance, 0);
  frame.viewProjection = viewProjection;
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

void WorldRenderer::SetViewport(Lighthouse::RenderPassDesc& pass, UVec2 size)
{
  pass.width = size.x;
  pass.height = size.y;
}

Mat4 WorldRenderer::ViewProjection(entt::entity entity) const
{
  const auto& registry = m_world->Registry();
  const auto& camera = registry.get<Player>(entity).cameraView;
  const auto& transform = registry.get<Transform>(entity);
  const auto size = camera.GetResolution();
  if (!(camera.verticalFov > 0 && camera.verticalFov < 180) ||
      !(camera.nearPlane > 0 && camera.farPlane > camera.nearPlane))
    throw std::invalid_argument("Invalid camera projection settings");
  const Mat4 view = glm::inverse(glm::translate(Mat4(1), transform.GetPos()) *
                                 glm::mat4_cast(transform.GetRot()));
  return glm::perspectiveRH_NO(glm::radians(camera.verticalFov),
                               float(size.x) / float(size.y), camera.nearPlane,
                               camera.farPlane) *
         view;
}

void WorldRenderer::ReleaseDraws()
{
  auto& device = *m_renderer->GetRenderDevice();
  m_entities.Release();
  for (const auto& lighting : m_lighting)
  {
    if (lighting.set)
      device.DestroyDescriptorSet(lighting.set);
    if (lighting.lights)
      device.DestroyDescriptorSet(lighting.lights);
  }
  m_lighting.clear();
  for (auto buffer : m_lightBuffers)
    if (buffer)
      device.DestroyBuffer(buffer);
  m_lightBuffers.clear();
  for (auto set : m_outputSets)
    if (set)
      device.DestroyDescriptorSet(set);
  m_outputSets.clear();
}
} // namespace EESG
