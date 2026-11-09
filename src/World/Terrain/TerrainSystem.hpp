#pragma once
#include "TerrainObserverManager.hpp"
#include "TerrainOctree.hpp"
#include <Lighthouse/Engine/Rendering/Graph/RenderGraph.hpp>
#include <array>
#include <functional>
#include <string>

namespace EESG
{
using namespace Lighthouse;
class TerrainSystem
{
public:
  TerrainSystem() = default;
  ~TerrainSystem();
  TerrainSystem(const TerrainSystem&) = delete;
  TerrainSystem& operator=(const TerrainSystem&) = delete;
  void BuildGraph(Lighthouse::RenderGraph&, Lighthouse::IRenderDevice&,
                  TerrainObserverManager&);
  // Changes take effect at the next graph execution; planet bounds are not
  // modeled yet.
  void SetRootCoverage(TerrainRootCoverage coverage);
  TerrainRootCoverage GetRootCoverage() const
  {
    return coverage_;
  }
  void PollReadbacks();
  void Release();
  bool InspectorEnabled() const
  {
    return inspectorEnabled_;
  }
  void EnableInspector(bool enabled)
  {
    inspectorEnabled_ = enabled;
  }
  const std::shared_ptr<const TerrainOctree>& Snapshot() const
  {
    return snapshot_;
  }
  const std::string& Diagnostic() const
  {
    return diagnostic_;
  }
  uint64_t SkippedReadbacks() const
  {
    return skippedReadbacks_;
  }

private:
  void Initialize(Lighthouse::IRenderDevice&);
  std::vector<std::byte> ObserverBytes(const TerrainObserverManager&);
  void Compute(Lighthouse::RenderGraphExecutePassContext&,
               Lighthouse::BufferHandle, Lighthouse::BufferHandle,
               Lighthouse::BufferHandle, Lighthouse::BufferHandle, uint32_t,
               uint32_t, Lighthouse::GraphicsPipelineHandle);
  struct Readback
  {
    Lighthouse::BufferHandle buffer{};
    Lighthouse::SemaphoreHandle semaphore;
    uint64_t value{}, revision{};
    bool pending{};
  };
  Lighthouse::IRenderDevice* device_{};
  Lighthouse::BufferHandle terrain_{};
  Lighthouse::DescriptorSetLayoutHandle setLayout_{};
  Lighthouse::PipelineLayoutHandle layout_{};
  Lighthouse::ShaderModuleHandle residencyShader_{};
  Lighthouse::GraphicsPipelineHandle residency_{};
  std::array<Readback, 3> readbacks_{};
  TerrainRootCoverage coverage_;
  bool inspectorEnabled_ = false;
  uint64_t revision_{}, skippedReadbacks_{};
  std::shared_ptr<const TerrainOctree> snapshot_;
  std::string diagnostic_;
};
} // namespace EESG
