#include "MaterialRayDevice.hpp"
#include "Materials/Materials.hpp"

#include "World/Terrain/TerrainMeshPool.hpp"
#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <Lighthouse/Backends/Render/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Backends/Render/Vulkan/VulkanBackend.hpp>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

using namespace EESG;
static void Check(bool condition)
{
  if (!condition)
    throw std::runtime_error("Material check failed");
}
template <class F> static void Rejects(F function)
{
  bool rejected = false;
  try
  {
    function();
  }
  catch (const std::exception&)
  {
    rejected = true;
  }
  Check(rejected);
}

void TestLoading(Lighthouse::IRenderDevice& device)
{
  Materials materials(device);
  const auto solid = materials.Load("materials/SolidColor.glsl");
  Check(solid->model == SurfaceModel::Unlit);
  Check(bool(solid->gBufferPipeline));
  Check(!solid->rayTracingPipeline);
  Check(device.IsValid(solid->graphicsLayout));
  Check(device.IsValid(solid->frameLayout));
  Check(solid->source.find("struct UnlitSurface") != std::string::npos);
  Check(solid->source.find("evaluateMaterial") != std::string::npos);
  const auto terrain = materials.Load("materials/Terrain.material.json");
  Check(terrain->model == SurfaceModel::Lit);
  Check(terrain->gBufferPipeline != solid->gBufferPipeline);
  Check(solid->shadingModel->attributes == std::vector{GBufferAttribute::Color});
  const auto lit = materials.Load("materials/Helmet.material.json");
  Check(lit->model == SurfaceModel::Lit);
  Check(terrain->shadingModel == lit->shadingModel);
  Check(lit->shadingModel != solid->shadingModel);
  Check(lit->shadingModel->attributes.front() == GBufferAttribute::Color);
  Check(lit->shadingModel->attributes.size() == 5);
  Check(device.IsValid(lit->shadingModel->deferredLightingPipeline.lightingLayout));
  Check(materials.Load(solid->path) == solid);
  Check(materials.Load("materials/../materials/SolidColor.glsl") == solid);
  Rejects([&] { materials.Load("materials/DoesNotExist.glsl"); });

  const auto temporary =
      std::filesystem::temp_directory_path() /
      ("eesg-material-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".glsl");
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup()
    {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } cleanup{temporary};
  // A wrong return type must fail, and failure must not poison the cache.
  {
    std::ofstream file(temporary);
    file << "float evaluateMaterial(SurfaceInput s, MaterialParameters p) { "
            "return 1.0; }";
  }
  Rejects([&] { materials.Load(temporary); });
  {
    std::ofstream file(temporary);
    file << "UnlitSurface evaluateMaterial(SurfaceInput s, MaterialParameters "
            "p) { return UnlitSurface(p.baseColor.rgb, p.baseColor.a); }";
  }
  Check(materials.Load(temporary)->model == SurfaceModel::Unlit);
}

void TestInstanceLifetime(Lighthouse::IRenderDevice& device)
{
  std::shared_ptr<const MaterialInstance> retained;
  {
    Materials materials(device);
    retained = materials.Load("materials/SolidColor.glsl")->Instantiate();
  }
  Check(device.IsValid(retained->parametersSet));
  Check(device.IsValid(retained->parametersBuffer));
  Check(device.IsValid(retained->material->graphicsLayout));
  const auto buffer = retained->parametersBuffer;
  const auto set = retained->parametersSet;
  retained.reset();
  Check(!device.IsValid(buffer));
  Check(!device.IsValid(set));
}

// The real backends currently advertise rayTracing=false. Explicit creation
// must reject rather than returning a fake usable pipeline.
void TestUnsupportedRayTracing(Lighthouse::IRenderDevice& device)
{
  Check(!device.GetCapabilities().rayTracing);
  Rejects([&] { device.CreateRayTracingPipeline({}); });
}

void TestTerrainPixels(
    Lighthouse::IRenderDevice& device,
    const std::filesystem::path& path = "materials/Terrain.material.json",
    const MaterialLoadOptions& options = {})
{
  using namespace Lighthouse;
  Materials materials(device);
  const auto material = materials.Load(path);
  const auto instance = material->Instantiate(options);
  WorldTerrain terrain;
  // Two source chunks on opposite sides of x=0 share one geometry buffer.
  terrain.set_voxel({-4, 0, 0}, {1, 1});
  terrain.set_voxel({3, 0, 0}, {1, 1});
  TerrainMeshPool pool(device);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  do {
    pool.Update(terrain);
    Check(std::chrono::steady_clock::now()<deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while(pool.Stats().pending || pool.Stats().chunks!=2);
  const MaterialFrameData camera{};
  const auto uniform = device.CreateBuffer(
      {.size = sizeof(camera),
       .usage = BufferUsage::Uniform,
       .initialData = std::as_bytes(std::span(&camera, 1))});
  const auto set =
      device.CreateDescriptorSet({.layout = material->frameLayout});
  const std::array writes{DescriptorWrite{
      .binding = 0,
      .resource =
          DescriptorBufferInfo{.buffer = uniform, .range = sizeof(camera)}}};
  device.UpdateDescriptorSet(set, writes);
  std::vector<ImageHandle> attributes;
  std::vector<ImageViewHandle> views;
  for (const auto attribute : material->shadingModel->attributes)
  {
    const auto format = GBufferAttributes[static_cast<size_t>(attribute)].format;
    attributes.push_back(device.CreateImage({.width = 128, .height = 128, .format = format,
        .usage = {ImageUsage::ColorAttachment, ImageUsage::Sampled, ImageUsage::TransferSource}}));
    views.push_back(device.CreateImageView({.image = attributes.back(), .format = format}));
  }
  const auto image = device.CreateImage({.width = 128, .height = 128, .format = ImageFormat::RGBA16f,
      .usage = {ImageUsage::ColorAttachment, ImageUsage::TransferSource}});
  const auto view = device.CreateImageView({.image = image, .format = ImageFormat::RGBA16f});
  const auto depth =
      device.CreateImage({.width = 128,
                          .height = 128,
                          .format = ImageFormat::Depth24Stencil8,
                          .usage = {ImageUsage::DepthStencilAttachment}});
  const auto depthView =
      device.CreateImageView({.image = depth, .format = ImageFormat::Depth24Stencil8});
  auto commands = device.CreateCommandList(QueueType::Graphics);
  commands->Begin();
  commands->BeginRendering({.depthAttachment = depthView,
                            .clearColor = true, .clearDepth = true,
                            .width = 128, .height = 128, .colorAttachments = views});
  commands->SetStencil(StencilMode::Write, 1);
  commands->BindPipeline(material->gBufferPipeline);
  commands->BindDescriptorSet(0, set);
  commands->BindDescriptorSet(1, instance->parametersSet);
  pool.Draw(*commands);
  commands->EndRendering();
  commands->End();
  device.Submit(*commands);
  const auto sampler = device.CreateSampler({});
  const auto& lighting = material->shadingModel->deferredLightingPipeline;
  const auto lightingSet = device.CreateDescriptorSet({.layout = lighting.attributesLayout});
  std::vector<DescriptorWrite> lightingWrites;
  for (size_t i = 0; i < views.size(); ++i)
    lightingWrites.push_back({.binding = static_cast<uint32_t>(material->shadingModel->attributes[i]),
        .resource = DescriptorImageInfo{views[i], sampler}});
  device.UpdateDescriptorSet(lightingSet, lightingWrites);
  // Lit materials validate their G-buffer albedo here; full lighting is
  // exercised by the WorldRenderer GPU image tests.
  if (material->model == SurfaceModel::Unlit)
  {
  commands->Clear();
  commands->Begin();
  commands->BeginRendering({.colorAttachment = view, .depthAttachment = depthView,
      .clearColor = true, .width = 128, .height = 128, .depthReadOnly = true});
  commands->SetStencil(StencilMode::Equal, 1);
  commands->BindPipeline(lighting.pipeline);
  commands->BindDescriptorSet(0, lightingSet);
  commands->Draw(PrimitiveTopology::TriangleList, 3, 0);
  commands->EndRendering();
  commands->End();
  device.Submit(*commands);
  }
  const auto readback = device.CreateImage({.width=128,.height=128,.format=ImageFormat::RGBA8,
      .usage={ImageUsage::TransferSource,ImageUsage::TransferDestination}});
  commands->Clear();
  commands->Begin();
  const Lighthouse::BlitRegion region{
      .srcSubresource={ImageAspect::Color,0,0,1}, .srcOffsets={{{0,0,0},{128,128,1}}},
      .dstSubresource={ImageAspect::Color,0,0,1}, .dstOffsets={{{0,0,0},{128,128,1}}}};
  commands->BlitImage(material->model==SurfaceModel::Lit ? attributes.front() : image,readback,region,ImageFilter::Nearest);
  commands->End();
  device.Submit(*commands);
  const auto pixels = device.ReadImage(readback);
  for (auto x : {36u, 92u})
    Check(std::to_integer<unsigned>(pixels[(60*128+x)*4+1]) > 20 ||
          std::to_integer<unsigned>(pixels[(68*128+x)*4+1]) > 20);
  Check(std::to_integer<unsigned>(pixels[(64*128+64)*4+1]) == 0);
  device.DestroyImage(readback);
  device.WaitIdle();
  device.DestroyDescriptorSet(lightingSet);
  device.DestroySampler(sampler);
  for (auto v : views) device.DestroyImageView(v);
  for (auto image : attributes) device.DestroyImage(image);
  device.DestroyDescriptorSet(set);
  device.DestroyBuffer(uniform);
  device.DestroyImageView(depthView);
  device.DestroyImage(depth);
  device.DestroyImageView(view);
  device.DestroyImage(image);
}

void TestRayPipelineContract(Lighthouse::IRenderDevice& device)
{
  MaterialRayDevice rayDevice(device);
  std::shared_ptr<const Material> retained;
  {
    Materials materials(rayDevice);
    retained = materials.Load("materials/SolidColor.glsl");
    Check(retained->rayTracingPipeline.has_value());
    Check(rayDevice.created == 1);
    Check(materials.Load(retained->path) == retained);
    Check(rayDevice.created == 1);
  }
  Check(rayDevice.destroyed == 0); // External owner retains device resources.
  retained.reset();
  Check(rayDevice.destroyed == 1);
  {
    Materials materials(rayDevice);
    const auto lit = materials.Load("materials/Helmet.material.json");
    Check(lit->rayTracingPipeline.has_value());
    Check(lit->shadingModel->attributes.size() == 5);
  }
  {
    Materials materials(rayDevice);
    rayDevice.failRayCreation = true;
    Rejects([&] { materials.Load("materials/SolidColor.glsl"); });
    Check(!device.IsValid(rayDevice.lastFrame));
    Check(!device.IsValid(rayDevice.lastLayout));
    rayDevice.failRayCreation = false;
    Check(materials.Load("materials/SolidColor.glsl")
              ->rayTracingPipeline.has_value());
  }
  Check(rayDevice.created == rayDevice.destroyed);
}

void TestJsonMaterials(Lighthouse::IRenderDevice& device)
{
  using namespace Lighthouse;
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("eesg-json-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup()
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};
  auto write = [&](const std::string& file, const std::string& text)
  {
    std::ofstream stream(directory / file);
    stream << text;
  };
  write("Surface.glsl", R"glsl(
UnlitSurface evaluateMaterial(SurfaceInput s, MaterialParameters p) {
    vec4 texel = SampleTexture2D(p.albedo, s.uv * p.uvScale);
    float enabled = p.enabled && p.mode == -2 && p.count == 3u ? 1.0 : 0.0;
    return UnlitSurface(texel.rgb * p.tint.rgb * p.gain * enabled + p.offset, texel.a);
})glsl");
  write("Surface.json", R"json({
    "shader":"Surface.glsl", "base":"Unlit", "parameters":{
      "other":{"type":"texture2D"},
      "albedo":{"type":"texture2D"},
      "tint":{"type":"vec4","default":[1,1,1,1]},
      "uvScale":{"type":"vec2","default":[2,3]},
      "gain":{"type":"float","default":0.5},
      "offset":{"type":"vec3","default":[0,0,0]},
      "mode":{"type":"int","default":-2},
      "count":{"type":"uint","default":3},
      "enabled":{"type":"bool","default":true}
    }
  })json");
  const auto path = directory / "Surface.json";
  const std::array<std::byte, 4> red{std::byte{255}, std::byte{0}, std::byte{0},
                                     std::byte{255}};
  const std::array<std::byte, 4> green{std::byte{0}, std::byte{255},
                                       std::byte{0}, std::byte{255}};
  const auto redImage = device.CreateImage({.format = ImageFormat::RGBA8,
                                            .usage = {ImageUsage::Sampled},
                                            .initialData = red});
  const auto greenImage = device.CreateImage({.format = ImageFormat::RGBA8,
                                              .usage = {ImageUsage::Sampled},
                                              .initialData = green});
  const auto redView =
      device.CreateImageView({.image = redImage, .format = ImageFormat::RGBA8});
  const auto greenView = device.CreateImageView(
      {.image = greenImage, .format = ImageFormat::RGBA8});
  const auto sampler = device.CreateSampler({});
  MaterialLoadOptions options;
  options.parameters["gain"] = 1.0f;
  options.textures["other"] = {redView, sampler};
  {
    Materials materials(device);
    const auto definition = materials.Load(path);
    Rejects([&] { definition->Instantiate(); });
    Rejects([&] { definition->Instantiate(options); }); // Missing albedo.
    options.textures["albedo"] = {greenView, sampler};
    auto material = definition->Instantiate(options);
    Check(material->material->parameters.size() == 9);
    Check(material->textures.size() == 2);
    Check(material->material->source.find("SampleTexture2D") != std::string::npos);
    Check(materials.Load(path) == material->material);
    const auto second = definition->Instantiate(options);
    Check(second != material);
    Check(second->material == material->material);
    Check(second->parametersBuffer != material->parametersBuffer);
    Check(second->parametersSet != material->parametersSet);
    auto changed = options;
    changed.parameters["gain"] = 0.25f;
    {
      const auto other = definition->Instantiate(changed);
      Check(other != material);
      Check(other->material->gBufferPipeline == material->material->gBufferPipeline);
    }
    changed = options;
    changed.textures["albedo"] = {redView, sampler};
    {
      const auto other = definition->Instantiate(changed);
      Check(other != material);
      Check(other->material->gBufferPipeline == material->material->gBufferPipeline);
    }
    changed = options;
    changed.parameters["enabled"] = 1.0f;
    Rejects([&] { definition->Instantiate(changed); });
    changed = options;
    changed.parameters["uvScale"] = Vec3(1.0f);
    Rejects([&] { definition->Instantiate(changed); });
    changed = options;
    changed.parameters["typo"] = 0.5f;
    Rejects([&] { definition->Instantiate(changed); });
    changed = options;
    changed.textures["typo"] = {greenView, sampler};
    Rejects([&] { definition->Instantiate(changed); });
    write(
        "Invalid.json",
        R"({"shader":"Surface.glsl","parameters":{"albedo":{"type":"texture2D","default":"white.png"}}})");
    Rejects([&] { materials.Load(directory / "Invalid.json"); });
    write(
        "Invalid.json",
        R"({"shader":"Surface.glsl","parameters":{"albedo":{"type":"texture2D","default":null}}})");
    Rejects([&] { materials.Load(directory / "Invalid.json"); });
    write("Invalid.json", R"({"shader":"Surface.glsl","base":"Lit"})");
    Rejects([&] { materials.Load(directory / "Invalid.json"); });
    write("Invalid.json", "{ broken JSON");
    Rejects([&] { materials.Load(directory / "Invalid.json"); });
    write(
        "Invalid.json",
        R"({"shader":"Surface.glsl","parameters":{"uvScale":{"type":"vec2","default":[1]}}})");
    Rejects([&] { materials.Load(directory / "Invalid.json"); });
    TestTerrainPixels(device, path,
                      options); // Slot 1 must sample green, not red.
    MaterialRayDevice rayDevice(device);
    Materials rayMaterials(rayDevice);
    Check(rayMaterials.Load(path)->rayTracingPipeline.has_value());
  }
  device.WaitIdle();
  device.DestroySampler(sampler);
  device.DestroyImageView(redView);
  device.DestroyImageView(greenView);
  device.DestroyImage(redImage);
  device.DestroyImage(greenImage);
}

int main()
{
  for (bool vulkan : {false, true})
  {
    std::unique_ptr<Lighthouse::IGraphicsBackend> backend;
    if (vulkan)
      backend = std::make_unique<Lighthouse::VulkanBackend>();
    else
      backend = std::make_unique<Lighthouse::OpenGLBackend>();
    std::string diagnostic;
    if (!backend->Initialize(diagnostic))
      throw std::runtime_error(diagnostic);
    {
      auto device =
          backend->CreateDevice({.applicationName = "EESG material tests"});
      TestLoading(*device);
      TestInstanceLifetime(*device);
      TestUnsupportedRayTracing(*device);
      TestTerrainPixels(*device);
      TestRayPipelineContract(*device);
      TestJsonMaterials(*device);
    }
    backend->Shutdown();
  }
}
