#include "Lighthouse/Engine/Core/Types.hpp"
#include "Lighthouse/Engine/Rendering/Graph/RenderGraph.hpp"
#include "Lighthouse/Engine/Rendering/Graph/SimpleRenderGraphAllocator.hpp"
#include "Lighthouse/Engine/Rendering/IRenderCommandList.hpp"
#include "Lighthouse/Engine/Rendering/RenderScene.hpp"
#include "Lighthouse/Engine/Rendering/RenderTechnique.hpp"
#include "Lighthouse/Engine/Rendering/RenderTypes.hpp"
#include "Lighthouse/Engine/Rendering/RenderView.hpp"
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
using namespace Lighthouse;
class EESGApplication final : public Application {
public:
  void OnStart() override {
    m_allocator = std::make_unique<SimpleRenderGraphAllocator>(
        *GetRenderer().GetRenderDevice());
    auto &scene = CreateScene("Main");
    auto camera = scene.CreateEntity("Camera");
    camera.Add<Transform>();
    camera.Add<Camera>(Camera{70.0f});
    scene.SetActiveCamera(camera);

    const auto windowHandle = GetWindowManager().CreateWindow({
        .title = "EESG",
        .width = 1280,
        .height = 720,
        .eglSurface = true,
    });
    auto *window = GetWindowManager().GetWindow(windowHandle);
    window->Show();

    RenderViewDesc description;
    description.scene = scene.Handle();
    description.camera = CameraSource{camera};

    m_windowView =
        GetRenderer().CreateView<WindowView>(windowHandle, description);
    auto *view = GetRenderer().GetView<WindowView>(m_windowView);
    const auto acquire = view->AddTechnique<AcquireSwapchainImageTechnique>();
    const auto worldRender = view->AddTechnique<WorldRenderTechnique>();
    // const auto clear = view->AddTechnique<ClearSwapchainTechnique>(acquire);

#ifdef LIGHTHOUSE_IMGUI
    m_imgui = std::make_shared<ImGuiSession>(*window);
    const auto imgui = view->AddTechnique<ImGuiTechnique>(m_imgui, [] {
      ImGui::ShowDemoWindow();
      ImPlot::ShowDemoWindow();
    });
    // view->AddTechnique<PresentSwapchainImageTechnique>(imgui);
#else
    // view->AddTechnique<PresentSwapchainImageTechnique>(acquire);
#endif

    view->AddLambdaTechnique([&](TechniqueBuildContext &ctx) {
      struct CompositeData {
        RenderGraphResource color;
      };
      auto &builder = ctx.graph.AddPass<CompositeData>(
          "composite",
          [&](RenderGraph::Builder &builder, CompositeData &data) {
            builder.Read(view->GetTechnique(imgui)->GetPassData().image);
            builder.Read(view->GetTechnique(worldRender)
                             ->GetWorldRenderPassData()
                             .color);
            data.color =
                builder.Write(view->GetTechnique(acquire)->GetPassData().image);
          },
          [&ctx, worldRender, view](const CompositeData &data,
                                    FrameGraphPassResources &resources,
                                    IRenderCommandList &cmdList) {
            const auto &worldColor =
                view->GetTechnique(worldRender)->GetWorldRenderPassData().color;

            RenderGraphImage::Desc worldColorDesc =
                resources.getDescriptor<RenderGraphImage>(worldColor);
            ImportedRenderGraphImage::Desc acquireDesc =
                resources.getDescriptor<ImportedRenderGraphImage>(data.color);
            const RenderGraphImage &worldColorImage =
                resources.get<RenderGraphImage>(worldColor);
            const ImportedRenderGraphImage &acquireImageResource =
                resources.get<ImportedRenderGraphImage>(data.color);

            BlitRegion region{
                .srcSubresource = {ImageAspect::Color, 0, 0, 1},
                .srcOffsets = {{{0, 0, 0},
                                {worldColorDesc.width, worldColorDesc.height,
                                 1}}},
                .dstSubresource = {ImageAspect::Color, 0, 0, 1},
                .dstOffsets = {
                    {{0, 0, 0}, {acquireDesc.width, acquireDesc.height, 1}}}};
            ImageFilter filter = ImageFilter::Nearest;
            cmdList.BlitImage(worldColorImage.handle,
                              acquireImageResource.handle, region, filter);
          });
    });

    m_graph.emplace(GetRenderer().Compile({m_windowView}));
    std::cout << m_graph->Dot() << std::endl;
    m_commandList = GetRenderer().GetRenderDevice()->CreateCommandList(QueueType::Graphics);
  }

  void OnUpdate(float) override { 
    
    RenderGraphExecuteContext ctx{
        .allocator = m_allocator.get(),
        .commandList = m_commandList.get()
    };
    m_commandList->Clear();
    m_graph->Execute(&ctx);
    GetRenderer().GetRenderDevice()->Submit(*m_commandList.get());
    GetRenderer().GetRenderDevice()->Present(GetRenderer().GetView<WindowView>(m_windowView)->swapchain);

 }

private:
  RenderViewHandle m_windowView;
#ifdef LIGHTHOUSE_IMGUI
  std::shared_ptr<ImGuiSession> m_imgui;
#endif
  std::optional<RenderGraph> m_graph;
  std::unique_ptr<SimpleRenderGraphAllocator> m_allocator;
  std::unique_ptr<IRenderCommandList> m_commandList;
  RenderScene m_scene;
};

int main() {
  Config config{
      .name = "EESG",
      .graphics = {.selection = BackendSelection::FirstAvailable},
  };
  config.AddBackend<OpenGLBackend>();
  config.AddBackend<VulkanBackend>();
  return Run<EESGApplication>(config);
}
