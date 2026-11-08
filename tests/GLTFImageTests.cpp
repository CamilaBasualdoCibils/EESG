#include "World/Entity/Light.hpp"
#include "World/Entity/MeshRender.hpp"
#include "World/Entity/Transform.hpp"
#include "World/LightVolumes.hpp"
#include "World/WorldRenderer.hpp"
#include <Lighthouse/Engine/Assets/Assets.hpp>
#include <Lighthouse/Backends/Render/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Backends/Render/Vulkan/VulkanBackend.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <source_location>
#include <set>
#include <sstream>
#include <thread>
#include <stdexcept>

using namespace Lighthouse;
namespace
{
void Check(bool value,
           std::source_location location = std::source_location::current())
{
  if (!value)
    throw std::runtime_error("glTF image check failed at line " +
                             std::to_string(location.line()));
}
template <class F> void Rejects(F function)
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
struct Fixtures
{
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("eesg-gltf-images-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  Fixtures();
  ~Fixtures()
  {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  void Json(const std::string& name, const nlohmann::json& json)
  {
    std::ofstream(directory / name) << json.dump();
  }
};
const std::array<unsigned char, 74> png{
    137, 80,  78,  71, 13, 10, 26,  10, 0,  0,  0,   13,  73,  72, 68,
    82,  0,   0,   0,  2,  0,  0,   0,  1,  8,  6,   0,   0,   0,  244,
    34,  127, 138, 0,  0,  0,  17,  73, 68, 65, 84,  120, 156, 99, 224,
    18,  145, 211, 56, 49, 45, 229, 63, 0,  9,  10,  3,   38,  10, 143,
    2,   229, 0,   0,  0,  0,  73,  69, 78, 68, 174, 66,  96,  130};
const char* pngURI =
    "data:image/"
    "png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+"
    "KAAAAEUlEQVR4nGPgEpHTODEt5T8ACQoDJgqPAuUAAAAASUVORK5CYII=";
void Word(std::ofstream& file, uint32_t value)
{
  for (int i = 0; i < 4; ++i)
    file.put(char((value >> (8 * i)) & 255));
}
Fixtures::Fixtures()
{
  std::filesystem::create_directory(directory);
  {
    std::ofstream file(directory / "pixels.png", std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), png.size());
  }
  const auto shared = nlohmann::json::parse(R"({
      "asset": {"version": "2.0"},
      "images": [{"uri": "pixels.png"}],
      "textures": [{"source": 0}, {"source": 0}]
  })");
  Json("External.gltf", shared);
  auto embedded = shared;
  embedded["images"][0]["uri"] = pngURI;
  Json("DataURI.gltf", embedded);
  auto binary = shared;
  binary["images"][0] = {{"bufferView", 0}, {"mimeType", "image/png"}};
  binary["buffers"] = {{{"byteLength", png.size()}}};
  binary["bufferViews"] = {
      {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", png.size()}}};
  auto json = binary.dump();
  while (json.size() % 4)
    json += ' ';
  const uint32_t binSize = uint32_t((png.size() + 3) & ~size_t(3));
  {
    std::ofstream file(directory / "Embedded.glb", std::ios::binary);
    Word(file, 0x46546c67);
    Word(file, 2);
    Word(file, 12 + 8 + uint32_t(json.size()) + 8 + binSize);
    Word(file, uint32_t(json.size()));
    Word(file, 0x4e4f534a);
    file << json;
    Word(file, binSize);
    Word(file, 0x004e4942);
    file.write(reinterpret_cast<const char*>(png.data()), png.size());
    for (size_t i = png.size(); i < binSize; ++i)
      file.put(0);
  }
  auto invalid = shared;
  invalid["textures"][0]["source"] = 99;
  Json("BadSource.gltf", invalid);
  invalid = shared;
  invalid["images"][0]["uri"] = "Missing.png";
  Json("Missing.gltf", invalid);
  {
    std::ofstream(directory / "Broken.png") << "not a PNG";
  }
  invalid["images"][0]["uri"] = "Broken.png";
  Json("Broken.gltf", invalid);
  auto twoImages = shared;
  twoImages["images"].push_back({{"uri", "pixels.png"}});
  twoImages["textures"][1]["source"] = 1;
  Json("TwoImages.gltf", twoImages);
}
void TestUploads(IRenderDevice& device, const Fixtures& fixtures)
{
  for (auto file : {"External.gltf", "DataURI.gltf", "Embedded.glb"})
  {
    const auto asset = Assets::LoadGLTF(fixtures.directory / file);
    Check(asset.images.size() == 1 && asset.textures.size() == 2);
    Check(asset.textures[0].image == 0 && asset.textures[1].image == 0);
    const auto& data = asset.images[0];
    Check(data.GetResolution() == UVec2(2, 1));
    Check(data.GetFormat() == ImageFormat::RGBA8);
    constexpr std::array<unsigned, 8> expected{10,  20,  30,  40,
                                               200, 150, 100, 255};
    Check(data.GetData().size() == expected.size());
    auto copy = data;
    Check(copy.GetData().data() != data.GetData().data());
    const auto image = device.CreateImage(
        data.ToImageDesc({ImageUsage::Sampled, ImageUsage::TransferSource}));
    const auto pixels = device.ReadImage(image);
    Check(pixels.size() == expected.size());
    for (size_t i = 0; i < pixels.size(); ++i)
    {
      Check(std::to_integer<unsigned>(pixels[i]) == expected[i]);
      Check(pixels[i] == data.GetData()[i]);
    }
    device.DestroyImage(image);
  }
  for (auto file : {"BadSource.gltf", "Missing.gltf", "Broken.gltf"})
    Rejects([&] { Assets::LoadGLTF(fixtures.directory / file); });
  Check(Assets::LoadGLTF(fixtures.directory / "TwoImages.gltf").images.size() ==
        2);
  Rejects([] { ImageData({0, 1}, ImageFormat::RGBA8, {}); });
  Rejects(
      [] { ImageData({1, 1}, ImageFormat::RGBA8, std::vector<std::byte>(3)); });
  Rejects([] { ImageData({1, 1}, ImageFormat::Undefined, {}); });
  const auto meshPath =
      std::filesystem::path(EESG_SOURCE_ROOT) /
      "extern/Lighthouse-Engine/Tests/Fixtures/SimpleMeshes.gltf";
  const auto meshes = Assets::LoadGLTF(meshPath);
  Check(meshes.meshes.size() == 2 && meshes.images.empty() &&
        meshes.textures.empty());
}

void TestTexturedWorld(std::unique_ptr<IRenderDevice> device,
                       const Fixtures& fixtures)
{
  auto* gpu = device.get();
  EESG::World world("Texture test");
  Renderer renderer(std::move(device));
  const auto asset = Assets::LoadGLTF(fixtures.directory / "External.gltf");
  const auto image = gpu->CreateImage(asset.images[0].ToImageDesc());
  const auto view = gpu->CreateImageView(
      {.image = image, .format = asset.images[0].GetFormat()});
  const auto sampler =
      gpu->CreateSampler({.minification = SamplerFilter::Nearest,
                          .magnification = SamplerFilter::Nearest});
  std::vector<ImageHandle> dataImages;
  std::vector<ImageViewHandle> dataViews;
  const auto dataTexture = [&](std::array<uint8_t, 4> values)
  {
    const auto bytes = std::as_bytes(std::span(values));
    auto image = gpu->CreateImage({.format = ImageFormat::RGBA8,
                                   .usage = {ImageUsage::Sampled},
                                   .initialData = bytes});
    auto view =
        gpu->CreateImageView({.image = image, .format = ImageFormat::RGBA8});
    dataImages.push_back(image);
    dataViews.push_back(view);
    return view;
  };
  const auto mr = dataTexture({0, 255, 0, 255}); // Rough dielectric.
  const auto normal = dataTexture({128, 128, 255, 255});
  const auto white = dataTexture({255, 255, 255, 255});
  const auto black = dataTexture({0, 0, 0, 255});
  {
    EESG::Materials materials(*gpu);
    EESG::MaterialLoadOptions options;
    options.textures["albedo"] = {view, sampler};
    options.textures["metallicRoughness"] = {mr, sampler};
    options.textures["normalMap"] = {normal, sampler};
    options.textures["occlusion"] = {white, sampler};
    options.textures["emissive"] = {black, sampler};
    auto sun = world.CreateEntity("Directional light");
    sun.Add<EESG::Transform>();
    sun.Add<EESG::Light>();
    auto point = world.CreateEntity("Point light");
    point.Add<EESG::Transform>().SetPos({0, 0, 1});
    point.Add<EESG::Light>(EESG::Light{
        .type = EESG::LightType::Point, .intensity = 0, .range = 2});
    auto secondPoint = world.CreateEntity("Second point instance");
    secondPoint.Add<EESG::Transform>().SetPos({0,0,1});
    secondPoint.Add<EESG::Light>(EESG::Light{.type=EESG::LightType::Point,
        .color={0,1,0}, .intensity=0, .range=2});
    auto spot = world.CreateEntity("Spot light");
    spot.Add<EESG::Transform>().SetPos({0, 0, 1});
    spot.Add<EESG::Light>(
        EESG::Light{.type = EESG::LightType::Spot, .intensity = 0, .range = 2});
    const auto material =
        materials.Load("materials/Helmet.material.json")->Instantiate(options);
    Mesh3D mesh;
    mesh.vertices = {{{-0.8f, -0.8f, 0}, {0, 0, 1}, {0, 0}},
                     {{0.8f, -0.8f, 0}, {0, 0, 1}, {1, 0}},
                     {{0.8f, 0.8f, 0}, {0, 0, 1}, {1, 1}},
                     {{-0.8f, 0.8f, 0}, {0, 0, 1}, {0, 1}}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    auto entity = world.CreateEntity("Textured quad");
    entity.Add<EESG::Transform>();
    const auto meshHandle = renderer.UploadMesh(mesh);
    entity.Add<EESG::MeshRender>(meshHandle, material);
    const auto unlit =
        materials.Load("materials/SolidColor.glsl")->Instantiate();
    for (const auto position : {Vec3(-1.0f, 0, 0.2f), Vec3(1.0f, 0, -0.2f)})
    {
      auto other = world.CreateEntity("Unlit overlap");
      auto& transform = other.Add<EESG::Transform>();
      transform.SetPos(position);
      transform.SetScale(Vec3(0.45f));
      other.Add<EESG::MeshRender>(meshHandle, unlit);
    }
    auto player = world.CreateEntity("Player");
    player.Add<EESG::Transform>().SetPos({0, 0, 3.6f});
    player.Add<EESG::Player>().cameraView.SetResolution({128, 72});
    Rejects([&] { player.Get<EESG::Player>().cameraView.SetResolution({0, 72}); });
    auto secondPlayer = world.CreateEntity("Second player");
    secondPlayer.Add<EESG::Transform>().SetPos({1, 0, 3.6f});
    secondPlayer.Add<EESG::Player>().cameraView.SetResolution({64, 64});
    const auto technique = renderer.AddTechnique<EESG::WorldRenderer>(
        &renderer, &world);
    auto* worldRenderer = renderer.GetTechnique<EESG::WorldRenderer>(technique);
    worldRenderer->SetAmbientRadiance(Vec3(0));
    const auto output = gpu->CreateImage(
        {.width = 128,
         .height = 72,
         .format = ImageFormat::RGBA8,
         .usage = {ImageUsage::Sampled, ImageUsage::TransferSource,
                   ImageUsage::TransferDestination}});
    const auto outputView =
        gpu->CreateImageView({.image = output, .format = ImageFormat::RGBA8});
    std::vector<std::byte> pixels;
    const auto readback = renderer.AddLambdaTechnique(
        [&](TechniqueBuildContext& context)
        {
          struct Readback
          {
            RenderGraphResource image, secondImage, output;
          };
          auto* techniqueObject = static_cast<EESG::WorldRenderer*>(
              renderer.GetTechnique(technique.handle));
          const auto target = context.graph.Import(
              "Readback target",
              ImportedRenderGraphImage::Desc{
                  .width = 128,
                  .height = 72,
                  .format = ImageFormat::RGBA8,
                  .usage = {ImageUsage::TransferSource,
                            ImageUsage::TransferDestination}},
              ImportedRenderGraphImage{.viewHandle = outputView,
                                       .handle = output});
          context.graph.AddPass<Readback>(
              "Readback textured world",
              [&](RenderGraph::Builder& builder, Readback& data)
              {
                Check(player.Get<EESG::Player>().cameraView.GetOutput() !=
                      secondPlayer.Get<EESG::Player>().cameraView.GetOutput());
                data.image = builder.Read(player.Get<EESG::Player>().cameraView.GetOutput());
                data.secondImage = builder.Read(secondPlayer.Get<EESG::Player>().cameraView.GetOutput());
                data.output = builder.Write(target);
                builder.SetSideEffect();
              },
              [&](const Readback& data, FrameGraphPassResources& resources,
                  RenderGraphExecutePassContext& execute)
              {
                const auto& firstDesc = resources.getDescriptor<RenderGraphImage>(data.image);
                const auto& secondDesc = resources.getDescriptor<RenderGraphImage>(data.secondImage);
                const auto expected = secondPlayer.Get<EESG::Player>().cameraView.GetResolution();
                Check(firstDesc.width == 128 && firstDesc.height == 72);
                Check(secondDesc.width == expected.x && secondDesc.height == expected.y);
                const BlitRegion region{
                    .srcSubresource = {ImageAspect::Color, 0, 0, 1},
                    .srcOffsets = {{{0, 0, 0}, {128, 72, 1}}},
                    .dstSubresource = {ImageAspect::Color, 0, 0, 1},
                    .dstOffsets = {{{0, 0, 0}, {128, 72, 1}}}};
                execute.commandList->Begin();
                execute.commandList->BlitImage(
                    resources.get<RenderGraphImage>(data.image).handle,
                    resources.get<ImportedRenderGraphImage>(data.output).handle,
                    region, ImageFilter::Nearest);
                execute.commandList->End();
              });
        });
    renderer.Compile();
    // Resource versions may differ, but shared attributes must have one
    // underlying graph resource across both shading models.
    const auto dot = renderer.GetRenderGraph().Dot();
    for (const auto name :
         {"Color", "World normal", "World position", "Surface properties",
          "Emission", "Material IDs (depth/stencil)"})
    {
      std::set<std::string> resourceIDs;
      std::istringstream graph(dot);
      std::string line;
      while (std::getline(graph, line))
      {
        if (line.find(std::string("<B>") + name + "</B>") == std::string::npos)
          continue;
        const auto start = line.find("Index: ");
        Check(start != std::string::npos);
        const auto end = line.find("<BR/>", start);
        resourceIDs.insert(line.substr(start, end - start));
      }
      Check(resourceIDs.size() == 2); // One shared attribute per camera.
    }
    renderer.Render();
    pixels = gpu->ReadImage(output);
    Check(pixels.size() == 128 * 72 * 4);
    // Interior samples exercise texture decoding, shared G-buffer color,
    // stencil selection, and GGX/Smith/Schlick shading.
    const auto component = [&](unsigned x, unsigned c)
    { return std::to_integer<int>(pixels[(36 * 128 + x) * 4 + c]); };
    // At normal incidence and roughness=1: GGX D=1/pi, Smith V=1/4,
    // dielectric F=0.04. This gives an independent analytic reference.
    const auto litChannel = [](int value)
    {
      const float srgb = value / 255.f;
      const float linear = srgb <= 0.04045f
                               ? srgb / 12.92f
                               : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
      const float lit = (0.96f * linear + 0.01f) / 3.14159265f;
      return 255.f * (lit <= 0.0031308f
                          ? 12.92f * lit
                          : 1.055f * std::pow(lit, 1.f / 2.4f) - 0.055f);
    };
    Check(std::abs(component(53, 0) - litChannel(10)) <= 4);
    Check(std::abs(component(53, 1) - litChannel(20)) <= 4);
    Check(std::abs(component(74, 0) - litChannel(200)) <= 5);
    // Both models share Color: later Lit draws must preserve earlier Unlit
    // pixels. Near Unlit wins the overlap; far Unlit loses to Lit depth.
    Check(component(40, 0) > 250 && component(40, 1) > 250);
    Check(component(47, 0) > 250 && component(47, 1) > 250);
    Check(component(82, 0) < 200);
    Check(component(90, 0) > 250 && component(90, 1) > 250);
    const auto initial = pixels;
    // A second camera can move and resize without altering this camera's image.
    secondPlayer.Get<EESG::Transform>().SetPos({-1, 1, 2});
    secondPlayer.Get<EESG::Player>().cameraView.SetResolution({80, 48});
    renderer.Compile();
    renderer.Render();
    Check(gpu->ReadImage(output) == initial);
    player.Get<EESG::Transform>().SetPos({0.5f, 0, 3.6f});
    renderer.Render();
    Check(gpu->ReadImage(output) != initial);
    player.Get<EESG::Transform>().SetPos({0, 0, 3.6f});
    const auto render = [&]
    {
      renderer.Render();
      pixels = gpu->ReadImage(output);
    };
    const auto changeMaterial = [&](const EESG::MaterialLoadOptions& changed)
    {
      entity.Get<EESG::MeshRender>().material =
          material->material->Instantiate(changed);
      renderer.Compile();
      render();
    };
    // Light components update without rebuilding the graph. All types share
    // the same model pipeline and scene buffer; disabled/removed entries
    // vanish.
    sun.Get<EESG::Light>().intensity = 0;
    render();
    Check(component(74, 0) == 0 && component(74, 1) == 0);
    Check(component(40, 0) > 250); // Unlit is independent of scene lighting.
    point.Get<EESG::Light>().intensity = 0.3f;
    render();
    const int pointRed = component(74, 0);
    Check(pointRed > 10);
    const auto pointGreen = component(74,1);
    secondPoint.Get<EESG::Light>().intensity = 0.3f;
    render();
    Check(std::abs(component(74,0)-pointRed) <= 1);
    Check(component(74,1) > pointGreen);
    secondPoint.Get<EESG::Light>().enabled = false;
    point.Get<EESG::Light>().type = EESG::LightType::Spot;
    render();
    Check(component(74,0) > 10); // Live shape changes update the indirect batches.
    point.Get<EESG::Light>().type = EESG::LightType::Point;
    point.Get<EESG::Light>().range = 4; // Camera is inside the sphere.
    render();
    Check(component(74, 0) > pointRed);
    point.Get<EESG::Light>().range =
        200; // Exit faces extend past the camera far plane.
    render();
    Check(component(74, 0) > pointRed);
    point.Get<EESG::Light>().range = 0.1f;
    render();
    Check(component(74, 0) == 0);
    point.Get<EESG::Light>().range = 2;
    point.Get<EESG::Transform>().SetPos({0, 0, -1});
    render();
    Check(component(74, 0) == 0); // Light behind the surface.
    point.Get<EESG::Transform>().SetPos({0, 0, 1});
    point.Get<EESG::Light>().intensity = 0;
    spot.Get<EESG::Light>().intensity = 0.3f;
    render();
    Check(component(74, 0) > 10);
    point.Get<EESG::Light>().intensity = 0.3f;
    render();
    Check(component(74, 0) >
          pointRed); // Contributions accumulate in linear space.
    point.Get<EESG::Light>().enabled = false;
    spot.Get<EESG::Transform>().SetRotEuler({0, 3.14159265f, 0});
    render();
    Check(component(74, 0) == 0); // Outside the spotlight cone.
    spot.Get<EESG::Transform>().SetRotEuler({0, 0, 0});
    spot.Get<EESG::Light>().innerAngle = spot.Get<EESG::Light>().outerAngle;
    render();
    Check(component(74, 0) > 10); // Equal angles form a hard edge, not NaNs.
    spot.Get<EESG::Light>().enabled = false;
    sun.Get<EESG::Light>().intensity = 1;
    auto changed = options;
    changed.textures["metallicRoughness"] = {dataTexture({0, 255, 255, 255}),
                                             sampler};
    changeMaterial(changed);
    const int roughMetal = component(74, 0);
    Check(roughMetal <
          litChannel(200) - 20); // Blue channel removes dielectric diffuse.
    changed.textures["metallicRoughness"] = {dataTexture({0, 64, 255, 255}),
                                             sampler};
    changeMaterial(changed);
    Check(component(74, 0) >
          roughMetal + 20); // Green channel controls highlight width.
    changed = options;
    changed.textures["normalMap"] = {dataTexture({255, 128, 128, 255}),
                                     sampler};
    changeMaterial(changed);
    Check(component(74, 0) <
          10); // Tangent-space normal changes the world-space lobe.
    sun.Get<EESG::Light>().enabled = false;
    changed = options;
    changed.textures["emissive"] = {view, sampler};
    changeMaterial(changed);
    Check(std::abs(component(74, 0) - 200) <=
          3); // Emission works with no lights, decoded once.
    worldRenderer->SetAmbientRadiance(Vec3(1));
    changeMaterial(options);
    Check(std::abs(component(74, 0) - 200) <= 3);
    changed = options;
    changed.textures["occlusion"] = {black, sampler};
    changeMaterial(changed);
    Check(component(74, 0) == 0); // AO masks indirect diffuse.
    sun.Get<EESG::Light>().enabled = true;
    worldRenderer->SetAmbientRadiance(Vec3(0));
    changeMaterial(changed);
    Check(std::abs(component(74, 0) - litChannel(200)) <=
          5); // AO never masks direct light.
    entity.Get<EESG::MeshRender>().material = material;
    renderer.Compile();
    render();
    Check(pixels == initial);
    entity.Get<EESG::Transform>().SetPos({0.5f, 0, 0});
    renderer.Render();
    pixels = gpu->ReadImage(output);
    Check(pixels !=
          initial); // Updated model matrix reaches the material shader.
    // Terrain edits update draw ranges in one persistent buffer. The same
    // compiled graph must display newly uploaded chunks and remove them again.
    auto meshes = world.Registry().view<EESG::MeshRender, EESG::Transform>();
    for (auto id : meshes) meshes.get<EESG::Transform>(id).SetPos({1000,0,0});
    renderer.Render();
    const auto emptyTerrain = gpu->ReadImage(output);
    const auto terrainGraph = renderer.GetRenderGraph().Dot();
    world.GetTerrain().apply({{-1,-1,0,0},{252,252,0},6,{1,1}});
    world.GetTerrain().apply({{0,-1,0,0},{0,252,0},6,{1,1}});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do
    {
      worldRenderer->UpdateTerrainMeshes();
      Check(std::chrono::steady_clock::now()<deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (worldRenderer->TerrainStats().pending || worldRenderer->TerrainStats().chunks!=2);
    renderer.Render();
    Check(gpu->ReadImage(output)!=emptyTerrain);
    Check(renderer.GetRenderGraph().Dot()==terrainGraph);
    world.GetTerrain().clear();
    worldRenderer->UpdateTerrainMeshes();
    renderer.Render();
    Check(gpu->ReadImage(output)==emptyTerrain);
    Check(renderer.GetRenderGraph().Dot()==terrainGraph);
    renderer.RemoveTechnique(readback);
    renderer.RemoveTechnique(technique);
    renderer.GetRenderGraph().Clear();
    world.Registry().clear<EESG::MeshRender>();
    gpu->DestroyImageView(outputView);
    gpu->DestroyImage(output);
  }
  gpu->WaitIdle();
  gpu->DestroySampler(sampler);
  for (auto dataView : dataViews)
    gpu->DestroyImageView(dataView);
  for (auto dataImage : dataImages)
    gpu->DestroyImage(dataImage);
  gpu->DestroyImageView(view);
  gpu->DestroyImage(image);
}
} // namespace
int main()
{
  Fixtures fixtures;
  const auto volumes = EESG::Detail::MakeLightVolumes();
  Check(volumes[0].indices.size() == 3);
  for (size_t shape : {size_t{1}, size_t{2}})
  {
    const auto& mesh = volumes[shape];
    Check(mesh.indices.size() > 30);
    for (size_t i=0; i<mesh.indices.size(); i+=3)
    {
      const auto a = mesh.vertices.at(mesh.indices[i]).position;
      const auto b = mesh.vertices.at(mesh.indices[i + 1]).position;
      const auto c = mesh.vertices.at(mesh.indices[i + 2]).position;
      const auto n = glm::normalize(glm::cross(b - a, c - a));
      if (shape == 1)
        Check(glm::dot(n, a) >= 1.0f - 1e-5f);
      else
        for (int angle = 0; angle < 64; ++angle)
        {
          const float phi = angle * 6.2831853f / 64;
          for (float depth : {0.0f, 0.5f, 1.0f})
            Check(glm::dot(n, Vec3(std::cos(phi) * depth, std::sin(phi) * depth,
                                   -depth) -
                                  a) <= 1e-5f);
        }
    }
  }
  for (bool vulkan : {false, true})
  {
    std::unique_ptr<IGraphicsBackend> backend;
    if (vulkan)
      backend = std::make_unique<VulkanBackend>();
    else
      backend = std::make_unique<OpenGLBackend>();
    std::string diagnostic;
    if (!backend->Initialize(diagnostic))
      throw std::runtime_error(diagnostic);
    {
      auto device =
          backend->CreateDevice({.applicationName = "glTF GPU image tests"});
      std::cout << device->GetCapabilities().deviceName << '\n';
      TestUploads(*device, fixtures);
      const auto helmetPath =
          std::filesystem::path(EESG_SOURCE_ROOT) / "DamagedHelmet.gltf";
      if (std::filesystem::exists(helmetPath))
      {
        const auto helmet = Assets::LoadGLTF(helmetPath);
        Check(!helmet.meshes.empty() && helmet.images.size() == 5 &&
              helmet.textures.size() == 5);
      }
      TestTexturedWorld(std::move(device), fixtures);
    }
    backend->Shutdown();
  }
}
