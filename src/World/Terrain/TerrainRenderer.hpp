#pragma once
#include "World/PreparedGeometry.hpp"
#include "World/Terrain/TerrainMeshPool.hpp"
#include "World/World.hpp"

namespace EESG
{
class TerrainRenderer
{
public:
  TerrainRenderer(Lighthouse::Renderer& renderer, World& world,
                  Materials& materials);
  ~TerrainRenderer();
  TerrainRenderer(const TerrainRenderer&) = delete;
  TerrainRenderer& operator=(const TerrainRenderer&) = delete;
  void Prepare(Lighthouse::TechniqueBuildContext& context,
               const Mat4& viewProjection, PreparedGeometry& groups);
  void Draw(const GeometryGroup& group, const Mat4& viewProjection,
            FrameGraphPassResources& resources,
            Lighthouse::RenderGraphExecutePassContext& execute);
  void Update() { m_terrainPool.Update(m_world->GetTerrain()); }
  void SetMeshingThreadCount(size_t count) { m_terrainPool.SetMeshingThreadCount(count); }
  TerrainMeshStats Stats() const { return m_terrainPool.Stats(); }
  void Release(); // Caller must wait for GPU completion before releasing.
private:
  struct DrawResources
  {
    Lighthouse::BufferHandle uniform{};
    Lighthouse::DescriptorSetHandle set{};
    struct Slot
    {
      Lighthouse::DescriptorSetHandle set{};
      uint64_t marker = 0;
    };
    std::array<Slot, 8> slots;
    std::span<std::byte> mapped;
    size_t stride = 0, current = 0;
    Mat4 viewProjection{1};
  };
  Lighthouse::Renderer* m_renderer;
  World* m_world;
  std::vector<DrawResources> m_draws;
  Materials& m_materials;
  TerrainMeshPool m_terrainPool;
  std::shared_ptr<const MaterialInstance> m_terrainMaterial;
};
} // namespace EESG
