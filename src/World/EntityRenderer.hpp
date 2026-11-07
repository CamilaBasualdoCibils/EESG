#pragma once
#include "World/PreparedGeometry.hpp"
#include "World/World.hpp"

namespace EESG
{
class EntityRenderer
{
public:
  EntityRenderer(Lighthouse::Renderer& renderer, World& world);
  ~EntityRenderer();
  EntityRenderer(const EntityRenderer&) = delete;
  EntityRenderer& operator=(const EntityRenderer&) = delete;
  void Prepare(Lighthouse::TechniqueBuildContext& context,
               const Mat4& viewProjection, PreparedGeometry& groups);
  void Draw(const GeometryGroup& group, const Mat4& viewProjection,
            FrameGraphPassResources& resources,
            Lighthouse::RenderGraphExecutePassContext& execute);
  void Release(); // Caller must wait for GPU completion before releasing.
private:
  struct DrawResources
  {
    Lighthouse::BufferHandle uniform{};
    Lighthouse::DescriptorSetHandle set{};
  };
  Lighthouse::Renderer* m_renderer;
  World* m_world;
  std::vector<DrawResources> m_draws;
};
} // namespace EESG
