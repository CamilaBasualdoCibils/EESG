#include "TerrainRenderer.hpp"
#include <array>
#include <cstring>
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
    for (auto& slot : draw.slots)
    {
      if (slot.marker)
        device.ReleaseCompletionMarker(slot.marker);
      if (slot.set)
        device.DestroyDescriptorSet(slot.set);
    }
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
    const auto alignment = device.StreamingBufferAlignment();
    draw.stride = (sizeof(frame) + alignment - 1) / alignment * alignment;
    draw.uniform = device.CreateBuffer(
        {.size = draw.stride * draw.slots.size(),
         .usage = BufferUsage::Uniform,
         .memory = {.preferredLocation = MemoryLocation::HostLocal,
                    .requiredAccess =
                        MemoryAccess::CPUWrite | MemoryAccess::GPURead}});
    draw.mapped = device.MapStreamingBuffer(draw.uniform);
    std::memcpy(draw.mapped.data(), &frame, sizeof(frame));
    device.PublishStreamingWrites(draw.uniform, 0, sizeof(frame));
    draw.viewProjection = viewProjection;
    for (size_t i = 0; i < draw.slots.size(); ++i)
    {
      auto& slot = draw.slots[i];
      slot.set = device.CreateDescriptorSet(
          {.layout = instance->material->frameLayout});
      const std::array writes{DescriptorWrite{
          .binding = 0,
          .resource = DescriptorBufferInfo{.buffer = draw.uniform,
                                           .offset = i * draw.stride,
                                           .range = sizeof(frame)}}};
      device.UpdateDescriptorSet(slot.set, writes); // graph construction only
    }
    draw.set = draw.slots[0].set;
    return draw;
  };
  if (!m_terrainMaterial)
    m_terrainMaterial =
        m_materials.Load("materials/Terrain.material.json")->Instantiate();
  // The graph reads one stable buffer. Runtime chunk ranges can change
  // without replacing graph resources or recompiling any passes.
  {
    const auto draw =
        makeFrame(m_terrainMaterial, {.viewProjection = viewProjection});
    auto& group = groups[m_terrainMaterial->material->model];
    group.model = m_terrainMaterial->material->shadingModel;
    group.terrainSet = draw.set;
    for (auto buffer : {m_terrainPool.Buffer(), draw.uniform,
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
    auto& device = *m_renderer->GetRenderDevice();
    auto draw =
        std::find_if(m_draws.begin(), m_draws.end(), [&](const auto& entry)
                     { return entry.set == group.terrainSet; });
    if (draw == m_draws.end())
      throw std::logic_error("Terrain camera resources were released");
    if (draw->viewProjection != viewProjection)
    {
      for (size_t i = 0; i < draw->slots.size(); ++i)
      {
        if (i == draw->current)
          continue;
        auto& next = draw->slots[i];
        if (next.marker && !device.PollCompletionMarker(next.marker))
          continue;
        if (next.marker)
        {
          device.ReleaseCompletionMarker(next.marker);
          next.marker = 0;
        }
        draw->slots[draw->current].marker = device.InsertCompletionMarker();
        const MaterialFrameData frame{.viewProjection = viewProjection};
        std::memcpy(draw->mapped.data() + i * draw->stride, &frame,
                    sizeof(frame));
        device.PublishStreamingWrites(draw->uniform, i * draw->stride,
                                      sizeof(frame));
        draw->current = i;
        draw->viewProjection = viewProjection;
        break;
      }
      // If all slots are in flight, keep the last constants for this draw.
      // Never wait for the GPU or overwrite a slot it may still be reading.
    }
    commands.BindPipeline(m_terrainMaterial->material->gBufferPipeline);
    commands.BindDescriptorSet(0, draw->slots[draw->current].set);
    commands.BindDescriptorSet(1, m_terrainMaterial->parametersSet);
    m_terrainPool.Draw(commands);
  }
}
} // namespace EESG
