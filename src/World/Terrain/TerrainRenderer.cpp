#include "TerrainRenderer.hpp"
#include <array>
#include <stdexcept>
namespace EESG
{
using namespace Lighthouse;
TerrainRenderer::TerrainRenderer(Renderer& renderer, World& world,
                                 Materials& materials)
    : m_renderer(&renderer), m_world(&world), m_materials(materials),
      m_terrainPool(*renderer.GetRenderDevice())
{
}
TerrainRenderer::~TerrainRenderer()
{
  m_renderer->GetRenderDevice()->WaitIdle();
  Release();
}
void TerrainRenderer::Release()
{
  auto& device = *m_renderer->GetRenderDevice();
  for (const auto& draw : m_draws)
  {
    if (draw.set)
      device.DestroyDescriptorSet(draw.set);
    if (draw.uniform)
      device.DestroyBuffer(draw.uniform);
  }
  m_draws.clear();
}
void TerrainRenderer::Prepare(TechniqueBuildContext& context,
                              const Mat4& viewProjection,
                              PreparedGeometry& groups)
{
  auto& device = context.device;
  auto import = [&](const char* name, BufferHandle buffer)
  { return ImportGeometryBuffer(context, name, buffer); };
  auto makeFrame = [&](const std::shared_ptr<const MaterialInstance>& instance,
                       const MaterialFrameData& frame)
  {
    m_draws.emplace_back();
    auto& draw = m_draws.back();
    draw.uniform = device.CreateBuffer(
        {.size = sizeof(frame),
         .usage = BufferUsage::Uniform,
         .initialData = std::as_bytes(std::span(&frame, 1))});
    draw.set =
        device.CreateDescriptorSet({.layout = instance->material->frameLayout});
    const std::array writes{
        DescriptorWrite{.binding = 0,
                        .resource = DescriptorBufferInfo{
                            .buffer = draw.uniform, .range = sizeof(frame)}}};
    device.UpdateDescriptorSet(draw.set, writes);
    return draw;
  };
  if (!m_terrainMaterial)
    m_terrainMaterial =
        m_materials.Load("materials/Terrain.material.json")->Instantiate();
  m_terrainPool.Rebuild(m_world->GetTerrain());
  if (m_terrainPool.Vertices())
  {
    const auto draw =
        makeFrame(m_terrainMaterial, {.viewProjection = viewProjection});
    auto& group = groups[m_terrainMaterial->material->model];
    group.model = m_terrainMaterial->material->shadingModel;
    group.terrainSet = draw.set;
    for (auto buffer : {m_terrainPool.Vertices(), m_terrainPool.Indices(),
                        m_terrainPool.Indirect(), draw.uniform,
                        m_terrainMaterial->parametersBuffer})
      group.terrain.push_back(import("Terrain buffer", buffer));
  }
  // An empty world still clears a valid output through the Unlit passes.
  if (groups.empty())
    groups[SurfaceModel::Unlit].model =
        m_terrainMaterial->material->shadingModel;
}
void TerrainRenderer::Draw(const GeometryGroup& group,
                           const Mat4& viewProjection,
                           FrameGraphPassResources& resources,
                           RenderGraphExecutePassContext& execute)
{
  auto& commands = *execute.commandList;
  if (group.terrainSet)
  {
    const MaterialFrameData frame{.viewProjection = viewProjection};
    m_renderer->GetRenderDevice()->UpdateBuffer(
        resources.get<ImportedRenderGraphBuffer>(group.terrain[3]).handle, 0,
        std::as_bytes(std::span(&frame, 1)));
    commands.BindPipeline(m_terrainMaterial->material->gBufferPipeline);
    commands.BindDescriptorSet(0, group.terrainSet);
    commands.BindDescriptorSet(1, m_terrainMaterial->parametersSet);
    m_terrainPool.Draw(commands);
  }
}
} // namespace EESG
