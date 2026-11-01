#include "Lighthouse/Engine/Rendering/IRenderCommandList.hpp"
#include "Lighthouse/Engine/Rendering/RenderGraph.hpp"
#include "Lighthouse/Engine/Rendering/RenderScene.hpp"
#include "Lighthouse/Engine/Rendering/RenderTechnique.hpp"
#include "Lighthouse/Engine/Rendering/SwapchainTechniques.hpp"
#include "Lighthouse/Engine/Rendering/WorldRenderTechnique.hpp"
#include <Lighthouse/Engine/Engine.hpp>
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
        view->AddTechnique<Lighthouse::WorldRenderTechnique>();
    // const auto clear = view->AddTechnique<ClearSwapchainTechnique>(acquire);

#ifdef LIGHTHOUSE_IMGUI
    m_imgui = std::make_shared<Lighthouse::ImGuiSession>(*window);
    const auto imgui =
        view->AddTechnique<Lighthouse::ImGuiTechnique>(m_imgui, [] {
          ImGui::ShowDemoWindow();
          ImPlot::ShowDemoWindow();
        });
    //view->AddTechnique<Lighthouse::PresentSwapchainImageTechnique>(imgui);
#else
    //view->AddTechnique<Lighthouse::PresentSwapchainImageTechnique>(acquire);
#endif

    view->AddLambdaTechnique([&](Lighthouse::TechniqueBuildContext &ctx) {
      struct CompositeData {
        Lighthouse::RenderGraphResource color;
      };
      auto &builder = ctx.graph.AddPass<CompositeData>(
          "composite",
          [&](Lighthouse::RenderGraph::Builder &builder, CompositeData &data) {
            builder.Read(view->GetTechnique(imgui)->GetPassData().image);
            builder.Read(view->GetTechnique(worldRender)
                             ->GetWorldRenderPassData()
                             .color);
            data.color =
                builder.Write(view->GetTechnique(acquire)->GetPassData().image);
          },
          [](const CompositeData &data, FrameGraphPassResources &,
             Lighthouse::IRenderCommandList &device) {

          });
    });

    m_graph.emplace(GetRenderer().Compile({m_windowView}));
    std::cout << m_graph->Dot() << std::endl;
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
