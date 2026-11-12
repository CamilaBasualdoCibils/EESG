#pragma once
#include "World/World.hpp"
#include <Lighthouse/Extended/Rendering/ForwardRenderFeature.hpp>
#include <Lighthouse/Extended/Rendering/DeferredRenderFeature.hpp>
namespace EESG
{
using namespace Lighthouse;
class TerrainRenderer final : public Lighthouse::IForwardPreparationProvider,
                              public Lighthouse::IDeferredPreparationProvider
{
public:
  explicit TerrainRenderer(World& world) : world_(world) {}
  ~TerrainRenderer() override
  {
    world_.GetTerrain().Release();
  }
  void BuildGraph(Lighthouse::ForwardRenderContext& forward) override
  {
    BuildTerrain(forward.frame);
  }
  void BuildGraph(Lighthouse::DeferredRenderContext& deferred) override
  {
    BuildTerrain(deferred.frame);
  }

private:
  void BuildTerrain(Lighthouse::RenderFeatureContext& context)
  {
    // A shared world can feed multiple independently configured renderer views.
    if (builtId_ != context.buildId)
    {
      builtId_ = context.buildId;
      world_.GetTerrain().BuildGraph(context.technique.graph,
                                     context.technique.device,
                                     world_.GetTerrainObservers());
    }
  }

  World& world_;
  std::uint64_t builtId_{};
};
} // namespace EESG
