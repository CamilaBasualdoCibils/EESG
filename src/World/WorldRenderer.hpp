#pragma once
#include "Materials/Materials.hpp"
#include "World/Entity/Player.hpp"
#include "World/EntityRenderer.hpp"
#include "World/Terrain/TerrainRenderer.hpp"
#include <Lighthouse/Engine/Rendering/Renderer.hpp>
#include <array>

namespace EESG
{
class WorldRenderer final : public Lighthouse::IRenderTechnique
{
public:
  WorldRenderer(Lighthouse::Renderer* renderer, World* world,
                Lighthouse::RenderViewHandle window = {});
  ~WorldRenderer() override;
  void BuildGraph(Lighthouse::TechniqueBuildContext& context) override;
  void UpdateTerrainMeshes() { m_terrain.Update(); }
  void SetTerrainMeshingThreadCount(size_t count) { m_terrain.SetMeshingThreadCount(count); }
  TerrainMeshStats TerrainStats() const { return m_terrain.Stats(); }
  void SetAmbientRadiance(Vec3 radiance);
  void SetDeltaTime(float value) noexcept
  {
    m_deltaTime = value;
  }
  Lighthouse::RenderGraphResource GetOutput() const noexcept;

private:
  void BuildCamera(Lighthouse::TechniqueBuildContext&, entt::entity,
                   CameraView&);
  void EnsureLightVolumes();
  std::array<Lighthouse::DrawIndexedIndirectCommand, 3>
  VolumeCommands(const std::vector<LightData>& lights) const;
  void CreateOutputPipeline(Lighthouse::IRenderDevice& device);
  static bool Finite(Vec3 value);
  std::vector<LightData> GatherLights() const;
  std::vector<std::byte> LightingBytes(const std::vector<LightData>& lights,
                                       size_t capacity,
                                       const Mat4& viewProjection) const;
  static void SetViewport(Lighthouse::RenderPassDesc& pass, UVec2 size);
  Mat4 ViewProjection(entt::entity camera) const;
  void ReleaseDraws();
  struct LightingResources
  {
    std::shared_ptr<const ShadingModel> model;
    Lighthouse::DescriptorSetHandle set{}, lights{};
  };
  Lighthouse::Renderer* m_renderer;
  World* m_world;
  Lighthouse::RenderViewHandle m_window;
  float m_deltaTime{};
  Lighthouse::RenderGraphResource m_output{Lighthouse::kInvalidGraphResource};
  std::vector<LightingResources> m_lighting;
  Lighthouse::SamplerHandle m_sampler{};
  std::vector<Lighthouse::BufferHandle> m_lightBuffers;
  std::array<Lighthouse::RenderMeshHandle, 3> m_volumes{};
  Lighthouse::DescriptorSetLayoutHandle m_outputSetLayout{};
  Lighthouse::PipelineLayoutHandle m_outputLayout{};
  Lighthouse::GraphicsPipelineHandle m_outputPipeline{};
  Lighthouse::ShaderModuleHandle m_outputVertex{}, m_outputFragment{};
  std::vector<Lighthouse::DescriptorSetHandle> m_outputSets;
  Vec3 m_ambientRadiance{0.03f};
  Materials m_materials;
  EntityRenderer m_entities;
  TerrainRenderer m_terrain;
};
} // namespace EESG
