#include "EntityRenderer.hpp"
#include "World/Entity/MeshRender.hpp"
#include "World/Entity/Transform.hpp"
#include <Lighthouse/Engine/Assets/Mesh.hpp>
#include <array>
#include <stdexcept>
namespace EESG
{
using namespace Lighthouse;
EntityRenderer::EntityRenderer(Renderer& renderer, World& world)
    : m_renderer(&renderer), m_world(&world)
{
}
EntityRenderer::~EntityRenderer()
{
  m_renderer->GetRenderDevice()->WaitIdle();
  Release();
}
void EntityRenderer::Release()
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
void EntityRenderer::Prepare(TechniqueBuildContext& context,
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
  auto entities = m_world->Registry().view<Transform, MeshRender>();
  for (auto entity : entities)
  {
    const auto& component = entities.get<MeshRender>(entity);
    const auto* mesh = m_renderer->GetRenderMesh(component.mesh);
    if (!mesh || m_renderer->GetRenderMeshUploadStatus(component.mesh) !=
                     RenderMeshUploadStatus::Ready)
      continue;
    if (!component.material)
      throw std::logic_error(
          "EntityRenderer requires a MaterialInstance for each mesh");
    if (mesh->layout.bindings.size() != 1 || mesh->vertexBuffers.size() != 1 ||
        !mesh->indexBuffer ||
        mesh->indexBuffer->format != IndexFormat::UInt32 ||
        mesh->topology != PrimitiveTopology::TriangleList ||
        mesh->layout.bindings.front().stride != sizeof(Mesh3DVertex))
      throw std::logic_error(
          "EntityRenderer expects indexed Mesh3DVertex geometry");
    const auto instance = component.material;
    const auto draw = makeFrame(
        instance, {.viewProjection = viewProjection,
                   .model = entities.get<Transform>(entity).GetMatrix()});
    auto& group = groups[instance->material->model];
    group.model = instance->material->shadingModel;
    group.geometry.push_back(
        {import("World vertices", mesh->vertexBuffers.front().buffer),
         import("World indices", mesh->indexBuffer->buffer),
         import("Object frame", draw.uniform),
         import("Material parameters", instance->parametersBuffer),
         component.mesh, entity, draw.set, instance});
  }
}
void EntityRenderer::Draw(const GeometryGroup& group,
                          const Mat4& viewProjection,
                          FrameGraphPassResources& resources,
                          RenderGraphExecutePassContext& execute)
{
  auto& commands = *execute.commandList;
  for (const auto& draw : group.geometry)
  {
    auto& registry = m_world->Registry();
    if (!registry.valid(draw.entity) ||
        !registry.all_of<Transform, MeshRender>(draw.entity) ||
        registry.get<MeshRender>(draw.entity).material != draw.instance)
      continue;
    const auto* mesh = m_renderer->GetRenderMesh(draw.mesh);
    if (!mesh)
      continue;
    const MaterialFrameData frame{
        .viewProjection = viewProjection,
        .model = registry.get<Transform>(draw.entity).GetMatrix()};
    m_renderer->GetRenderDevice()->UpdateBuffer(
        resources.get<ImportedRenderGraphBuffer>(draw.uniform).handle, 0,
        std::as_bytes(std::span(&frame, 1)));
    commands.BindPipeline(draw.instance->material->gBufferPipeline);
    commands.BindDescriptorSet(0, draw.set);
    commands.BindDescriptorSet(1, draw.instance->parametersSet);
    commands.BindVertexBuffer(
        resources.get<ImportedRenderGraphBuffer>(draw.vertices).handle,
        mesh->layout.bindings.front().stride);
    commands.BindIndexBuffer(
        resources.get<ImportedRenderGraphBuffer>(draw.indices).handle);
    commands.DrawIndexedRange(
        mesh->indexCount,
        static_cast<uint32_t>(mesh->indexBuffer->offset / sizeof(uint32_t)),
        static_cast<int32_t>(mesh->vertexBuffers.front().offset /
                             mesh->layout.bindings.front().stride));
  }
}
} // namespace EESG
