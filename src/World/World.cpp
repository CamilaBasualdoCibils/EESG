#include "World/World.hpp"
#include "World/Entity/Player.hpp"
#include <Lighthouse/Extended/World/Entity/Transform.hpp>
namespace EESG {
using namespace Lighthouse;
using Lighthouse::Transform;
void World::UpdateTerrain() {
    auto cameras = Registry().view<Player, Transform>();
    for (auto entity : cameras)
      cameras.get<Player>(entity).cameraView.UpdateTerrainObserver(
          observers_,
          WorldPosition(cameras.get<Transform>(entity).GetPos()),
          cameras.get<Transform>(entity).GetRot());
    // Standalone CameraView components support non-player camera entities.
    auto views = Registry().view<TerrainCamera, Transform>();
    for (auto entity : views)
      views.get<TerrainCamera>(entity).UpdateTerrainObserver(
          observers_,
          WorldPosition(views.get<Transform>(entity).GetPos()),
          views.get<Transform>(entity).GetRot());
    terrain_.PollReadbacks();
}

}
