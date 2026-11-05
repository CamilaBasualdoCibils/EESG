#include "Lighthouse/Engine/Core/Types.hpp"
#include "Lighthouse/Engine/Rendering/Graph/IRenderGraphAllocator.hpp"
#include "Lighthouse/Engine/Rendering/Graph/RenderGraph.hpp"
#include "Lighthouse/Engine/Rendering/IRenderCommandList.hpp"
#include "Lighthouse/Engine/Rendering/RenderTechnique.hpp"
#include "Lighthouse/Engine/Rendering/RenderTypes.hpp"
#include "Lighthouse/Engine/Rendering/RenderView.hpp"
#include "Lighthouse/Engine/Rendering/WorldRenderTechnique.hpp"
#include <Lighthouse/Engine/Engine.hpp>
#include <Lighthouse/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Vulkan/VulkanBackend.hpp>

#ifdef LIGHTHOUSE_IMGUI
#include <Lighthouse/Engine/Rendering/ImGuiView.hpp>
#include <imgui.h>
#include <implot.h>
#endif

#include <memory>
#include <optional>
using namespace Lighthouse;
class EESGApplication final : public Application {
public:
  void OnStart() override {

    m_windowView = GetRenderer().CreateView<WindowView>(RenderWindowDesc{
        .title = "EESG",
        .width = 1280,
        .height = 720,
        .decorated = true,
    });
    auto *view = GetRenderer().GetView<WindowView>(m_windowView);


    const auto worldRender = GetRenderer().AddTechnique<WorldRenderTechnique>();

#ifdef LIGHTHOUSE_IMGUI
    m_imguiView = GetRenderer().CreateView<ImGuiView>([] {
      ImGui::ShowDemoWindow();
      ImPlot::ShowDemoWindow();
    });
#endif

    GetRenderer().AddLambdaTechnique([this, view,
                                      worldRender](TechniqueBuildContext &ctx) {
      struct CompositeData {
        RenderGraphResource source;
        RenderGraphResource color;
      };
      ctx.graph.AddPass<CompositeData>(
          "composite",
          [this, view, worldRender](RenderGraph::Builder &builder,
                                    CompositeData &data) {
            data.source = builder.Read(GetRenderer()
                                           .GetTechnique(worldRender)
                                           ->GetWorldRenderPassData()
                                           .color);
            data.color = builder.Write(view->GetAcquiredImageAsRGResource());
          },
          [view](const CompositeData &data, FrameGraphPassResources &resources,
                 RenderGraphExecutePassContext &context) {
            const auto &source = resources.get<RenderGraphImage>(data.source);
            const auto &desc =
                resources.getDescriptor<RenderGraphImage>(data.source);
            const auto &target = resources.get<ImportedWindowView>(data.color);
            const auto size = view->GetWindow()->GetFramebufferSize();
            BlitRegion region{
                .srcSubresource = {ImageAspect::Color, 0, 0, 1},
                .srcOffsets = {{{0, 0, 0}, {desc.width, desc.height, 1}}},
                .dstSubresource = {ImageAspect::Color, 0, 0, 1},
                .dstOffsets = {{{0, 0, 0}, {size.width, size.height, 1}}}};
            context.commandList->BlitImage(source.handle, target.currentImage,
                                           region, ImageFilter::Nearest);
          });
    });

    GetRenderer().Compile();
    std::cout << GetRenderer().GetRenderGraph().Dot() << std::endl;
  }

  void OnUpdate(float) override { GetRenderer().Render(); }

private:
  RenderViewHandle m_windowView;
#ifdef LIGHTHOUSE_IMGUI
  RenderViewHandle m_imguiView;
#endif
  std::optional<RenderGraph> m_graph;
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
