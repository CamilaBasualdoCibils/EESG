#pragma once
#include <Lighthouse/Extended/Rendering/IRenderFeature.hpp>
#include "World/World.hpp"
namespace EESG {
using namespace Lighthouse;
class TerrainRenderer final : public Lighthouse::IRenderPreparationProvider {
public:
 explicit TerrainRenderer(World& world) : world_(world) {}
 ~TerrainRenderer() override { world_.GetTerrain().Release(); }
 void BuildGraph(Lighthouse::RenderFeatureContext& context) override {
   // A shared world can feed multiple independently configured renderer views.
   if (builtId_ != context.buildId) {
     builtId_ = context.buildId;
     world_.GetTerrain().BuildGraph(context.technique.graph, context.technique.device,
                                   world_.GetTerrainObservers());
   }
 }
private:
 World& world_;
 std::uint64_t builtId_{};
};
}
