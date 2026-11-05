#include "Lighthouse/Engine/Assets/Assets.hpp"
#include "Lighthouse/Engine/Core/Application.hpp"
#include "Lighthouse/Engine/Core/Config.hpp"
#include "Lighthouse/Engine/Core/Types.hpp"
#include "Lighthouse/Engine/Graphics/GraphicsBackend.hpp"
#include "Lighthouse/Engine/Rendering/Graph/IRenderGraphAllocator.hpp"
#include "Lighthouse/Engine/Rendering/Graph/RenderGraph.hpp"
#include "Lighthouse/Engine/Rendering/IRenderCommandList.hpp"
#include "Lighthouse/Engine/Rendering/RenderTechnique.hpp"
#include "Lighthouse/Engine/Rendering/RenderTypes.hpp"
#include "Lighthouse/Engine/Rendering/RenderView.hpp"
#include "Lighthouse/Engine/Rendering/Renderer.hpp"
#include "World/Entity/Transform.hpp"
#include "World/World.hpp"
#include "World/WorldRenderer.hpp"
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
namespace EESG
{
class EESGApplication final : public Lighthouse::Application
{
public:
  void OnStart() override
  {

    m_windowView = GetRenderer().CreateView<Lighthouse::WindowView>(
        Lighthouse::RenderWindowDesc{
            .title = "EESG",
            .width = 1280,
            .height = 720,
            .decorated = true,
        });
    auto* view = GetRenderer().GetView<Lighthouse::WindowView>(m_windowView);

    const auto worldRender =
        GetRenderer().AddTechnique<WorldRenderer>(&GetRenderer(), &m_world);

#ifdef LIGHTHOUSE_IMGUI
    m_imguiView = GetRenderer().CreateView<Lighthouse::ImGuiView>(
        []
        {
          ImGui::ShowDemoWindow();
          ImPlot::ShowDemoWindow();
        });
#endif
    Lighthouse::GLTFAsset gltfAsset;
    Lighthouse::BufferHandle vertexBuffer, indexBuffer;
    size_t vBufferSize, indexBufferSize;
    gltfAsset = Lighthouse::Assets::LoadGLTF("../DamagedHelmet.gltf");
    const Lighthouse::Mesh3D& mesh = gltfAsset.meshes[0];
    std::span<const Lighthouse::Mesh3DVertex> vertexSpan(mesh.vertices);
    Lighthouse::BufferDesc vBuffer{

        .size = vertexSpan.size_bytes(),

        .usage = {Lighthouse::BufferUsage::Vertex},
        .initialData = std::as_bytes(vertexSpan),
        .memory =
            Lighthouse::MemoryDesc{
                .preferredLocation = Lighthouse::MemoryLocation::DeviceLocal,
                .requiredAccess = Lighthouse::MemoryAccess::GPURead,
            },
    };
    vBufferSize = vertexSpan.size_bytes();

    vertexBuffer = GetRenderer().GetRenderDevice()->CreateBuffer(vBuffer);
    std::span<const uint32_t> indexSpan(mesh.indices);
    Lighthouse::BufferDesc iBuffer{
        .size = indexSpan.size_bytes(),
        .usage = {Lighthouse::BufferUsage::Index},
        .initialData = std::as_bytes(indexSpan),
        .memory =
            Lighthouse::MemoryDesc{
                .preferredLocation = Lighthouse::MemoryLocation::DeviceLocal,
                .requiredAccess = Lighthouse::MemoryAccess::GPURead,
            },
    };
    indexBuffer = GetRenderer().GetRenderDevice()->CreateBuffer(iBuffer);
    indexBufferSize = indexSpan.size_bytes();
    



    GetRenderer().AddLambdaTechnique(
        [&](Lighthouse::TechniqueBuildContext& ctx)
        {
           Lighthouse::RenderGraphResource helmetvBufferImport= ctx.graph.Import(
        "Helmet VBuffer",
        Lighthouse::ImportedRenderGraphBuffer::Desc{
            .size = vBufferSize, .usage = Lighthouse::BufferUsage::Vertex},
        Lighthouse::ImportedRenderGraphBuffer{.handle = vertexBuffer});
        Lighthouse::RenderGraphResource helmetiBufferImport= ctx.graph.Import(
            "Helmet IBuffer",
            Lighthouse::ImportedRenderGraphBuffer::Desc{
                .size = indexBufferSize, .usage = Lighthouse::BufferUsage::Index},
            Lighthouse::ImportedRenderGraphBuffer{.handle = indexBuffer});

          struct CompositeData
          {
            Lighthouse::RenderGraphResource source;
            Lighthouse::RenderGraphResource color;
          };
          ctx.graph.AddPass<CompositeData>(
              "composite",
              [&](Lighthouse::RenderGraph::Builder& builder,
                            CompositeData& data)
              {
                /* data.source = builder.Read(GetRenderer()
                                               .GetTechnique(worldRender)
                                               ->GetWorldRenderPassData()
                                               .color); */
                data.color =
                    builder.Write(view->GetAcquiredImageAsRGResource());
                    builder.Read(helmetvBufferImport);
                    builder.Read(helmetiBufferImport);
              },
              [view](const CompositeData& data,
                     FrameGraphPassResources& resources,
                     Lighthouse::RenderGraphExecutePassContext& context)
              {
                /*      const auto &source =
                         resources.get<Lighthouse::RenderGraphImage>(data.source);
                 */
                /*                 const auto &desc =
                                    resources.getDescriptor<Lighthouse::RenderGraphImage>(
                                        data.source); */
                const auto& target =
                    resources.get<Lighthouse::ImportedWindowView>(data.color);
                const auto size = view->GetWindow()->GetFramebufferSize();
                /*                 Lighthouse::BlitRegion region{
                                    .srcSubresource =
                   {Lighthouse::ImageAspect::Color, 0, 0, 1}, .srcOffsets =
                   {{{0, 0, 0}, {desc.width, desc.height, 1}}}, .dstSubresource
                   = {Lighthouse::ImageAspect::Color, 0, 0, 1}, .dstOffsets =
                   {{{0, 0, 0}, {size.width, size.height, 1}}}}; */
                /*                 context.commandList->BlitImage(
                                    source.handle, target.currentImage, region,
                                    Lighthouse::ImageFilter::Nearest); */
              });
        });

    GetRenderer().Compile();
    std::cout << GetRenderer().GetRenderGraph().Dot() << std::endl;
  }

  void OnUpdate(float) override
  {
    GetRenderer().Render();
  }

private:
  Lighthouse::RenderViewHandle m_windowView;
#ifdef LIGHTHOUSE_IMGUI
  Lighthouse::RenderViewHandle m_imguiView;
#endif
  std::optional<Lighthouse::RenderGraph> m_graph;
  World m_world{"MainWorld"};
};
} // namespace EESG

int main()
{
  Lighthouse::Config config{
      .name = "EESG",
      .graphics = {.selection = Lighthouse::BackendSelection::FirstAvailable},
  };
  config.AddBackend<Lighthouse::OpenGLBackend>();
  config.AddBackend<Lighthouse::VulkanBackend>();
  return Run<EESG::EESGApplication>(config);
}
