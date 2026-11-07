#include "Materials/Materials.hpp"
#include "Materials/MaterialDefinition.hpp"

#include <Lighthouse/Engine/Assets/Mesh.hpp>
#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace EESG
{
namespace
{
std::string ReadSource(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Cannot open material GLSL: " + path.string());
  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad())
    throw std::runtime_error("Cannot read material GLSL: " + path.string());
  return text.str();
}
} // namespace

Material::~Material()
{
  if (gBufferPipeline || rayTracingPipeline)
    m_device.WaitIdle();
  if (rayTracingPipeline)
    m_device.DestroyRayTracingPipeline(*rayTracingPipeline);
  if (gBufferPipeline)
    m_device.DestroyPipeline(gBufferPipeline);
  if (m_fragment)
    m_device.DestroyModule(m_fragment);
  if (m_vertex)
    m_device.DestroyModule(m_vertex);
  if (m_rayLayout)
    m_device.DestroyPipelineLayout(m_rayLayout);
  if (graphicsLayout)
    m_device.DestroyPipelineLayout(graphicsLayout);
  if (parametersLayout)
    m_device.DestroyDescriptorSetLayout(parametersLayout);
  if (frameLayout)
    m_device.DestroyDescriptorSetLayout(frameLayout);
}

MaterialInstance::~MaterialInstance()
{
  auto& device = material->m_device;
  if (parametersSet || parametersBuffer)
    device.WaitIdle();
  if (parametersSet)
    device.DestroyDescriptorSet(parametersSet);
  if (parametersBuffer)
    device.DestroyBuffer(parametersBuffer);
}
std::shared_ptr<const MaterialInstance>
Material::Instantiate(const MaterialLoadOptions& options) const
{
  using namespace Lighthouse;
  const auto definition = Detail::ReadDefinition(path, options);
  if (definition.declarations != m_declarations ||
      definition.shader != m_shader || definition.model != model)
    throw std::runtime_error(
        "Material definition changed; reload before instantiation");
  auto instance = std::shared_ptr<MaterialInstance>(
      new MaterialInstance(shared_from_this()));
  instance->textures = definition.textures;
  for (const auto& texture : instance->textures)
    if (!m_device.IsValid(texture.sampler))
      throw std::runtime_error(
          "Texture sampler is invalid or belongs to another device");
  instance->parametersBuffer = m_device.CreateBuffer(
      {.size = definition.data.size() * sizeof(definition.data[0]),
       .usage = BufferUsage::Uniform,
       .initialData = std::as_bytes(std::span(definition.data))});
  instance->parametersSet =
      m_device.CreateDescriptorSet({.layout = parametersLayout});
  std::vector<DescriptorWrite> writes{
      {.binding = 0,
       .resource = DescriptorBufferInfo{.buffer = instance->parametersBuffer,
                                        .range = definition.data.size() *
                                                 sizeof(definition.data[0])}}};
  for (uint32_t i = 0; i < instance->textures.size(); ++i)
    writes.push_back({.binding = i + 1, .resource = instance->textures[i]});
  m_device.UpdateDescriptorSet(instance->parametersSet, writes);
  return instance;
}
ShadingModel::~ShadingModel()
{
  auto& p = deferredLightingPipeline;
  if (p.pipeline)
    m_device.WaitIdle();
  if (p.pipeline)
    m_device.DestroyPipeline(p.pipeline);
  if (m_fragment)
    m_device.DestroyModule(m_fragment);
  if (m_vertex)
    m_device.DestroyModule(m_vertex);
  if (p.layout)
    m_device.DestroyPipelineLayout(p.layout);
  if (p.lightingLayout)
    m_device.DestroyDescriptorSetLayout(p.lightingLayout);
  if (p.attributesLayout)
    m_device.DestroyDescriptorSetLayout(p.attributesLayout);
}
std::shared_ptr<const ShadingModel>
Materials::LoadShadingModel(SurfaceModel model)
{
  using namespace Lighthouse;
  if (auto it = m_models.find(model); it != m_models.end())
    return it->second;
  auto shading = std::shared_ptr<ShadingModel>(new ShadingModel(m_device));
  shading->model = model;
  shading->attributes = {GBufferAttribute::Color};
  if (model == SurfaceModel::Lit)
  {
    shading->attributes.push_back(GBufferAttribute::WorldNormal);
    shading->attributes.push_back(GBufferAttribute::WorldPosition);
    shading->attributes.push_back(GBufferAttribute::SurfaceProperties);
    shading->attributes.push_back(GBufferAttribute::Emission);
  }
  auto& pipeline = shading->deferredLightingPipeline;
  DescriptorSetLayoutDesc layout;
  for (const auto attribute : shading->attributes)
    layout.bindings.push_back({.binding = static_cast<uint32_t>(attribute),
                               .type = DescriptorType::CombinedImageSampler,
                               .count = 1,
                               .stages = ShaderStageFlags::Fragment});
  pipeline.attributesLayout = m_device.CreateDescriptorSetLayout(layout);
  PipelineLayoutDesc pipelineLayout{.sets = {pipeline.attributesLayout}};
  if (model == SurfaceModel::Lit)
  {
    pipeline.lightingLayout = m_device.CreateDescriptorSetLayout(
        {.bindings = {{.binding = 0,
                       .type = DescriptorType::StorageBuffer,
                       .count = 1,
                       .stages = ShaderStageFlags::Vertex |
                                 ShaderStageFlags::Fragment}}});
    pipelineLayout.sets.push_back(pipeline.lightingLayout);
  }
  pipeline.layout = m_device.CreatePipelineLayout(pipelineLayout);
  const std::string base = model == SurfaceModel::Lit ? "Lit" : "Unlit";
  std::string vertexSource =
      model == SurfaceModel::Lit
          ? "#version 450\n" +
                ReadSource(m_resourceRoot /
                           "materials/passes/EvaluateLight.glsl") +
                "\n" +
                ReadSource(m_resourceRoot /
                           "materials/passes/LightVolume.vert.glsl")
          : ReadSource(m_resourceRoot /
                       "materials/passes/DeferredLighting.vert.glsl");
  shading->m_vertex = m_device.CreateShaderModule(
      ShaderCompiler::Compile(vertexSource, {.stage = ShaderStage::Vertex}));
  std::string lightingSource;
  if (model == SurfaceModel::Lit)
    lightingSource =
        "#version 450\n" +
        ReadSource(m_resourceRoot / "materials/passes/EvaluateLight.glsl") +
        "\n" + ReadSource(m_resourceRoot / "materials/passes/LitBRDF.glsl") +
        "\n";
  lightingSource += ReadSource(
      m_resourceRoot / ("materials/passes/" + base + ".deferred.frag.glsl"));
  shading->m_fragment = m_device.CreateShaderModule(ShaderCompiler::Compile(
      lightingSource, {.stage = ShaderStage::Fragment}));
  VertexLayout volumeLayout;
  if (model == SurfaceModel::Lit)
    volumeLayout = {
        .bindings = {{.binding = 0, .stride = sizeof(Mesh3DVertex)}},
        .attributes = {{.location = 0,
                        .binding = 0,
                        .format = VertexFormat::Float3,
                        .offset = offsetof(Mesh3DVertex, position)},
                       {.location = 1,
                        .binding = 0,
                        .format = VertexFormat::Float3,
                        .offset = offsetof(Mesh3DVertex, normal)}}};
  pipeline.pipeline = m_device.CreatePipeline(
      {.vertexShader = shading->m_vertex,
       .fragmentShader = shading->m_fragment,
       .vertexLayout = volumeLayout,
       .colorFormats = {ImageFormat::RGBA16f},
       .depthFormat = ImageFormat::Depth24Stencil8,
       .layout = pipeline.layout,
       .blend = {.enabled = model == SurfaceModel::Lit,
                 .sourceColorFactor = BlendFactor::One,
                 .destinationColorFactor = BlendFactor::One,
                 .colorOperation = BlendOperation::Add,
                 .sourceAlphaFactor = BlendFactor::One,
                 .destinationAlphaFactor = BlendFactor::One,
                 .alphaOperation = BlendOperation::Add}});
  if (!pipeline.pipeline)
    throw std::runtime_error(
        "Device returned an invalid deferred lighting pipeline");
  m_models.emplace(model, shading);
  return shading;
}

Materials::Materials(Lighthouse::IRenderDevice& device)
    : Materials(device, EESG_RESOURCE_ROOT)
{
}
Materials::Materials(Lighthouse::IRenderDevice& device,
                     std::filesystem::path resourceRoot)
    : m_device(device),
      m_resourceRoot(std::filesystem::absolute(std::move(resourceRoot)))
{
}

std::shared_ptr<const Material>
Materials::Load(const std::filesystem::path& path)
{
  using namespace Lighthouse;
  const auto resolved = std::filesystem::canonical(
      path.is_absolute() ? path : m_resourceRoot / path);
  Detail::MaterialDefinition definition;
  try
  {
    definition = Detail::ReadDefinition(resolved, {}, true);
  }
  catch (const std::exception& error)
  {
    throw std::runtime_error("Invalid material definition '" +
                             resolved.string() + "': " + error.what());
  }
  const auto key = resolved.generic_string();
  if (const auto found = m_loaded.find(key); found != m_loaded.end())
    return found->second;

  auto material = std::shared_ptr<Material>(new Material(m_device));
  material->path = resolved;
  material->parameters = definition.parameters;
  material->model = definition.model;
  material->shadingModel = LoadShadingModel(material->model);
  material->m_declarations = definition.declarations;
  material->m_shader = definition.shader;
  const std::string base =
      definition.model == SurfaceModel::Lit ? "Lit" : "Unlit";
  material->source =
      ReadSource(m_resourceRoot / ("materials/" + base + ".glsl")) + "\n" +
      definition.declarations + "\n#line 1 1\n" + ReadSource(definition.shader);
  try
  {
    const auto vertex = ShaderCompiler::Compile(
        ReadSource(m_resourceRoot / "materials/passes/Unlit.vert.glsl"),
        {.stage = ShaderStage::Vertex});
    const auto fragment = ShaderCompiler::Compile(
        "#version 450\n" + material->source + "\n#line 1 2\n" +
            ReadSource(m_resourceRoot /
                       ("materials/passes/" + base + ".frag.glsl")),
        {.stage = ShaderStage::Fragment});
    material->frameLayout = m_device.CreateDescriptorSetLayout(
        {.bindings = {{.binding = 0,
                       .type = DescriptorType::UniformBuffer,
                       .count = 1,
                       .stages = ShaderStageFlags::Vertex}}});
    const bool rayTracing = m_device.GetCapabilities().rayTracing;
    const auto stages =
        rayTracing ? ShaderStageFlags::Fragment | ShaderStageFlags::ClosestHit
                   : ShaderStageFlags::Fragment;
    DescriptorSetLayoutDesc parameterLayout;
    parameterLayout.bindings.push_back({.binding = 0,
                                        .type = DescriptorType::UniformBuffer,
                                        .count = 1,
                                        .stages = stages});
    for (uint32_t i = 0; i < definition.textures.size(); ++i)
    {
      parameterLayout.bindings.push_back(
          {.binding = i + 1,
           .type = DescriptorType::CombinedImageSampler,
           .count = 1,
           .stages = stages});
    }
    material->parametersLayout =
        m_device.CreateDescriptorSetLayout(parameterLayout);
    material->graphicsLayout = m_device.CreatePipelineLayout(
        {.sets = {material->frameLayout, material->parametersLayout}});
    material->m_vertex = m_device.CreateShaderModule(vertex);
    material->m_fragment = m_device.CreateShaderModule(fragment);
    std::vector<ImageFormat> formats;
    for (auto attribute : material->shadingModel->attributes)
      formats.push_back(
          GBufferAttributes[static_cast<size_t>(attribute)].format);
    material->gBufferPipeline = m_device.CreatePipeline(
        {.vertexShader = material->m_vertex,
         .fragmentShader = material->m_fragment,
         .vertexLayout =
             {.bindings = {{.binding = 0, .stride = sizeof(Mesh3DVertex)}},
              .attributes = {{.location = 0,
                              .binding = 0,
                              .format = VertexFormat::Float3,
                              .offset = offsetof(Mesh3DVertex, position)},
                             {.location = 1,
                              .binding = 0,
                              .format = VertexFormat::Float3,
                              .offset = offsetof(Mesh3DVertex, normal)},
                             {.location = 2,
                              .binding = 0,
                              .format = VertexFormat::Float2,
                              .offset = offsetof(Mesh3DVertex, uv)}}},
         .colorFormats = formats,
         .depthFormat = ImageFormat::Depth24Stencil8,
         .layout = material->graphicsLayout});
    if (!material->gBufferPipeline)
      throw std::runtime_error("Device returned an invalid graphics pipeline");

    if (m_device.GetCapabilities().rayTracing)
    {
      const auto hit = ShaderCompiler::Compile(
          "#version 460\n#extension GL_EXT_ray_tracing : require\n#define "
          "EESG_RAY_TRACING\n" +
              std::string(material->model == SurfaceModel::Lit
                              ? "#define UnlitSurface LitSurface\n"
                              : "") +
              material->source + "\n#line 1 2\n" +
              ReadSource(m_resourceRoot / "materials/passes/Unlit.rchit.glsl"),
          {.stage = ShaderStage::ClosestHit});
      material->m_rayLayout = m_device.CreatePipelineLayout(
          {.sets = {material->frameLayout, material->parametersLayout}});
      const auto pipeline = m_device.CreateRayTracingPipeline(
          {.closestHit = {.stage = ShaderStage::ClosestHit,
                          .bytecode = std::as_bytes(hit.GetSPIRV()),
                          .entryPoint = hit.GetEntryPoint()},
           .layout = material->m_rayLayout});
      if (!pipeline)
        throw std::runtime_error(
            "Device returned an invalid ray-tracing pipeline");
      material->rayTracingPipeline = pipeline;
    }
  }
  catch (const std::exception& error)
  {
    throw std::runtime_error(
        "Cannot load material '" + resolved.string() +
        "' (GLSL source 0: base, 1: material, 2: pass):\n" + error.what());
  }
  m_loaded.emplace(key, material);
  return material;
}
} // namespace EESG
