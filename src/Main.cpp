#include "Lighthouse/Engine/Rendering/RenderScene.hpp"
#include "Lighthouse/Engine/Rendering/RenderTechnique.hpp"
#include "Lighthouse/Engine/Rendering/WorldRenderer.hpp"
#include <Lighthouse/Engine/Engine.hpp>
#include <Lighthouse/Engine/Rendering/SwapchainTechniques.hpp>
#include <Lighthouse/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Vulkan/VulkanBackend.hpp>
#include <implot.h>

#ifdef LIGHTHOUSE_IMGUI
#include <Lighthouse/Engine/Rendering/ImGuiTechnique.hpp>
#include <imgui.h>
#endif

#include <memory>
#include <optional>
#include <stdexcept>

class ClearSwapchainTechnique final : public Lighthouse::IRenderTechnique {
public:
  struct PassData {
    Lighthouse::GraphImageHandle image{Lighthouse::InvalidGraphImage};
  };

  explicit ClearSwapchainTechnique(
      Lighthouse::TechniqueRef<Lighthouse::AcquireSwapchainImageTechnique>
          source)
      : m_source(source) {}

  const PassData &GetPassData() const noexcept { return m_passData; }

  void BuildGraph(Lighthouse::TechniqueBuildContext &ctx) override {
    const auto target = ctx.GetTechnique(m_source).GetPassData().image;
    if (target == Lighthouse::InvalidGraphImage)
      throw std::logic_error("Clear pass requires an acquired swapchain image");

    m_passData = ctx.graph.AddPass<PassData>(
        "clear window",
        [target](Lighthouse::RenderGraph::Builder &builder, PassData &data) {
          data.image = builder.Write(target);
        },
        [](const PassData &data, FrameGraphPassResources &resources,
           Lighthouse::IRenderDevice &device) {
          const auto &image =
              resources.get<Lighthouse::ImportedRenderGraphImage>(data.image);
          const auto &desc =
              resources.getDescriptor<Lighthouse::ImportedRenderGraphImage>(
                  data.image);
          auto commands =
              device.CreateCommandList(Lighthouse::QueueType::Graphics);
          Lighthouse::RenderPassDesc pass;
          pass.colorAttachment = image.view;
          pass.width = desc.width;
          pass.height = desc.height;
          pass.clearColor = true;
          pass.clearColorValue = {0.12f, 0.14f, 0.18f};
          commands->BeginRendering(pass);
          commands->EndRendering();
          device.Submit(*commands);
        });
  }

private:
  Lighthouse::TechniqueRef<Lighthouse::AcquireSwapchainImageTechnique> m_source;
  PassData m_passData;
};

class EESGApplication final : public Lighthouse::Application {
public:
  void OnStart() override {
    auto &scene = CreateScene("Main");
    auto camera = scene.CreateEntity("Camera");
    camera.Add<Lighthouse::Transform>();
    camera.Add<Lighthouse::Camera>(Lighthouse::Camera{70.0f});
    scene.SetActiveCamera(camera);

    const auto windowHandle = GetWindowManager().CreateWindow({
        .title = "EESG",
        .width = 1280,
        .height = 720,
        .eglSurface = true,
    });
    auto *window = GetWindowManager().GetWindow(windowHandle);
    window->Show();

    Lighthouse::RenderViewDesc description;
    description.scene = scene.Handle();
    description.camera = Lighthouse::CameraSource{camera};

    m_windowView = GetRenderer().CreateView<Lighthouse::WindowView>(
        windowHandle, description);
    auto *view = GetRenderer().GetView<Lighthouse::WindowView>(m_windowView);

    const auto acquire =
        view->AddTechnique<Lighthouse::AcquireSwapchainImageTechnique>();
    const auto worldRender =
        view->AddTechnique<Lighthouse::WorldRenderTechnique>(
            m_deferredWorldRenderer, &m_scene,Lighthouse::TechniqueRef<>);
    // const auto clear = view->AddTechnique<ClearSwapchainTechnique>(acquire);

#ifdef LIGHTHOUSE_IMGUI
    m_imgui = std::make_shared<Lighthouse::ImGuiSession>(*window);
    const auto imgui =
        view->AddTechnique<Lighthouse::ImGuiTechnique>(m_imgui, acquire, [] {
          ImGui::ShowDemoWindow();
          ImPlot::ShowDemoWindow();
        });
    view->AddTechnique<Lighthouse::PresentSwapchainImageTechnique>(imgui);
#else
    view->AddTechnique<Lighthouse::PresentSwapchainImageTechnique>(acquire);
#endif

    m_graph.emplace(GetRenderer().Compile({m_windowView}));
  }

  void OnUpdate(float) override {
    if (auto *device = GetRenderer().GetRenderDevice(); device && m_graph)
      m_graph->Execute(*device);
  }

private:
  Lighthouse::RenderViewHandle m_windowView;
#ifdef LIGHTHOUSE_IMGUI
  std::shared_ptr<Lighthouse::ImGuiSession> m_imgui;
#endif
  Lighthouse::DeferredWorldRenderer m_deferredWorldRenderer;
  std::optional<Lighthouse::RenderGraph> m_graph;
  Lighthouse::RenderScene m_scene;
};

int main() {
  Lighthouse::Config config{
      .name = "EESG",
      .graphics = {.selection = Lighthouse::BackendSelection::FirstAvailable},
  };
  config.AddBackend<Lighthouse::OpenGLBackend>();
  config.AddBackend<Lighthouse::VulkanBackend>();
  return Lighthouse::Run<EESGApplication>(config);
}
