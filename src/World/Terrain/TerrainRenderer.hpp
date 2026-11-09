#pragma once
#include <Lighthouse/Extended/Rendering/IRenderExtension.hpp>
#include "World/World.hpp"
namespace EESG {
using namespace Lighthouse;
class TerrainRenderer final : public Lighthouse::IRenderExtension {
public:
 explicit TerrainRenderer(World& world) : world_(world) {}
 ~TerrainRenderer() override { world_.GetTerrain().Release(); }
 void BuildGraph(Lighthouse::RenderGraph& graph, Lighthouse::RenderStage stage,
                 Lighthouse::RenderContext& context) override {
   // Residency is world-wide. Register it once even for multiple camera views.
   if (stage == Lighthouse::RenderStage::BeforeGeometry &&
       builtId_ != context.buildId) {
     builtId_ = context.buildId;
     world_.GetTerrain().BuildGraph(graph, context.technique.device, world_.GetTerrainObservers());
   }
 }
private:
 World& world_;
 std::uint64_t builtId_{};
};
}
