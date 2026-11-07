#include "Lighthouse/Engine/Core/Application.hpp"
#include "Math/Types.hpp"
#include "World/Entity/Transform.hpp"
#include "World/WorldRenderer.hpp"
#include <Lighthouse/Engine/Assets/Assets.hpp>
#include <Lighthouse/Engine/Engine.hpp>
#include <Lighthouse/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Vulkan/VulkanBackend.hpp>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <limits>

#ifdef LIGHTHOUSE_IMGUI
#include <Lighthouse/Engine/Rendering/ImGuiView.hpp>
#include <imgui.h>
#include <implot.h>
#endif
#include <boost/program_options.hpp>

namespace EESG
{
class EESGApplication final : public Lighthouse::IApplication
{
public:
  ~EESGApplication() override
  {
    // Release the technique while its World is still alive.
    GetRenderer().RemoveTechnique(m_worldRenderer);
  }
  void OnStart() override
  {
    auto& renderer = GetRenderer();
    m_windowView = renderer.CreateWindowView(
        {.title = "EESG", .width = 1280, .height = 720, .decorated = true});
    const auto asset = Lighthouse::Assets::LoadGLTF("../DragonAttenuation.gltf");
    if (asset.meshes.empty() || asset.meshes[0].vertices.empty())
      throw std::runtime_error("DamagedHelmet.gltf has no mesh");
    const Lighthouse::Mesh3D& mesh3D = asset.meshes[1];
    const auto meshHandle = renderer.UploadMesh(mesh3D);
    auto dragon = m_world.CreateEntity("dragon");
    auto& transform = dragon.Add<Transform>();
    // Fit the mesh to the temporary fixed camera, applying the glTF node's
    // quarter-turn around X. Geometry in the shared pool remains unmodified.
    Vec3 minimum(std::numeric_limits<float>::max());
    Vec3 maximum(std::numeric_limits<float>::lowest());
    for (const auto& vertex : mesh3D.vertices)
    {
      minimum = glm::min(minimum, vertex.position);
      maximum = glm::max(maximum, vertex.position);
    }
    const auto extent = maximum - minimum;
    const float scale = 2.0f / std::max({extent.x, extent.y, extent.z, 0.001f});
    const Quat rotation(glm::radians(Vec3(90.0f, 0.0f, 0.0f)));
    transform.SetRot(rotation);
    transform.SetScale(Vec3(scale));
    transform.SetPos(-(rotation * ((minimum + maximum) * (0.5f * scale))));
    dragon.Add<MeshRender>(meshHandle);
    // Three blocky spheres in view of the temporary camera, spanning chunk
    // boundaries so the demo also exercises pooled chunk draws.
    for (const auto center : {Vec3(-1.05f, -0.65f, 0.3f),
                              Vec3(0.95f, -0.6f, 0.1f), Vec3(0.0f, 0.9f, -0.5f)})
    {
      constexpr float radius = 0.45f;
      const auto low = WorldTerrain::voxel_position(Dvec3(center - Vec3(radius)));
      const auto high = WorldTerrain::voxel_position(Dvec3(center + Vec3(radius)));
      for (auto z = low[2]; z <= high[2]; ++z)
        for (auto y = low[1]; y <= high[1]; ++y)
          for (auto x = low[0]; x <= high[0]; ++x)
          {
            const Vec3 point = (Vec3(x, y, z) + Vec3(0.5f)) * float(WorldTerrain::voxel_width);
            if (glm::dot(point - center, point - center) <= radius * radius)
              m_world.GetTerrain().set_voxel({x, y, z}, {1, 1});
          }
    }
    m_worldRenderer =
        renderer
            .AddTechnique<WorldRenderer>(&renderer, &m_world, UVec2{1280, 720})
            .handle;
    renderer.AddLambdaTechnique(
        [this](Lighthouse::TechniqueBuildContext& context)
        {
          using namespace Lighthouse;
          auto* worldRenderer = static_cast<WorldRenderer*>(
              context.renderer.GetTechnique(m_worldRenderer));
          auto* windowView = context.renderer.GetView<WindowView>(m_windowView);
          struct PresentData
          {
            RenderGraphResource source, destination;
          };
          context.graph.AddPass<PresentData>(
              "Present world",
              [worldRenderer, windowView](RenderGraph::Builder& builder,
                                          PresentData& data)
              {
                data.source = builder.Read(worldRenderer->GetOutput());
                data.destination =
                    builder.Write(windowView->GetAcquiredImageAsRGResource());
              },
              [windowView](const PresentData& data,
                           FrameGraphPassResources& resources,
                           RenderGraphExecutePassContext& execute)
              {
                const auto& source =
                    resources.get<RenderGraphImage>(data.source);
                const auto& description =
                    resources.getDescriptor<RenderGraphImage>(data.source);
                const auto& destination =
                    resources.get<ImportedWindowView>(data.destination);
                const auto size = windowView->GetWindow()->GetFramebufferSize();
                const auto width =
                    std::min(size.width, size.height * description.width /
                                             description.height);
                const auto height =
                    std::min(size.height, size.width * description.height /
                                              description.width);
                const auto x = (size.width - width) / 2;
                const auto y = (size.height - height) / 2;
                const BlitRegion region{
                    .srcSubresource = {ImageAspect::Color, 0, 0, 1},
                    .srcOffsets = {{{0, 0, 0},
                                    {description.width, description.height,
                                     1}}},
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
        });
#ifdef LIGHTHOUSE_IMGUI
    renderer.CreateView<Lighthouse::ImGuiView>(
        [this, dragon = dragon]() mutable
        {
          ImGui::ShowDemoWindow();
          ImPlot::ShowDemoWindow();

          ImGui::Begin("Dragon Info");
          auto* worldRenderer = static_cast<WorldRenderer*>(
              GetRenderer().GetTechnique(m_worldRenderer));
          const auto renderSize = worldRenderer->GetRenderResolution();
          int resolution[]{static_cast<int>(renderSize.x),
                           static_cast<int>(renderSize.y)};
          if (ImGui::DragInt2("Render resolution", resolution, 16.0f, 64, 4096))
          {
            worldRenderer->SetRenderResolution(
                {static_cast<unsigned>(std::clamp(resolution[0], 64, 4096)),
                 static_cast<unsigned>(std::clamp(resolution[1], 64, 4096))});
            m_recompile = true;
          }

          Vec3 position = dragon.Get<Transform>().GetPos();
          Vec3 rotation =
              glm::degrees(glm::eulerAngles(dragon.Get<Transform>().GetRot()));
          Vec3 scale = dragon.Get<Transform>().GetScale();
          if (ImGui::DragFloat3("Position", &position.x, 0.1f))
            dragon.Get<Transform>().SetPos(position);
          if (ImGui::DragFloat3("Rotation", &rotation.x, 0.1f))
            dragon.Get<Transform>().SetRot(glm::quat(glm::radians(rotation)));
          if (ImGui::DragFloat3("Scale", &scale.x, 0.1f))
            dragon.Get<Transform>().SetScale(scale);

          ImGui::End();
        });
#endif
    const auto size = renderer.GetView<Lighthouse::WindowView>(m_windowView)
                          ->GetWindow()
                          ->GetFramebufferSize();
    m_width = size.width;
    m_height = size.height;
    renderer.Compile();
    m_terrainRevision = m_world.GetTerrain().revision();
    std::cout << renderer.GetRenderGraph().Dot() << '\n';
  }
  void OnUpdate(float) override
  {
    auto& renderer = GetRenderer();
    const auto size = renderer.GetView<Lighthouse::WindowView>(m_windowView)
                          ->GetWindow()
                          ->GetFramebufferSize();
    if (!size.width || !size.height)
      return;
    if (m_world.GetTerrain().revision() != m_terrainRevision || m_recompile || size.width != m_width || size.height != m_height)
    {
      renderer.Compile();
      m_terrainRevision = m_world.GetTerrain().revision();
      m_recompile = false;
      m_width = size.width;
      m_height = size.height;
    }
    renderer.Render();
  }

private:
  World m_world{"MainWorld"};
  Lighthouse::RenderViewHandle m_windowView{};
  Lighthouse::RenderTechniqueHandle m_worldRenderer{};
  std::uint32_t m_width{}, m_height{};
  uint64_t m_terrainRevision{};
  bool m_recompile{};
};
} // namespace EESG

int main(int argc, char** argv)
{
  boost::program_options::options_description desc{"Options"};
  desc.add_options()("vulkan", "Use Vulkan backend")("opengl",
                                                     "Use OpenGL backend");
  boost::program_options::variables_map vm;
  boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
  boost::program_options::notify(vm);
  Lighthouse::Config config{
      .name = "EESG",
      .graphics = {.selection = Lighthouse::BackendSelection::FirstAvailable}};
  if (vm.count("vulkan"))
    config.AddBackend<Lighthouse::VulkanBackend>();
  if (vm.count("opengl"))
    config.AddBackend<Lighthouse::OpenGLBackend>();
  if (!vm.count("vulkan") && !vm.count("opengl")) {
    config.AddBackend<Lighthouse::VulkanBackend>();
    config.AddBackend<Lighthouse::OpenGLBackend>();
  }
  return Run<EESG::EESGApplication>(config);
}
