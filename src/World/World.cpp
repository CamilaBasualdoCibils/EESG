#include "World/World.hpp"
#include "World/Entity/Player.hpp"
#include "World/Entity/Transform.hpp"

#include <stdexcept>
#include <atomic>
#include <utility>

namespace EESG {
namespace {
std::uint32_t NextWorldIndex() {
    static std::atomic<std::uint32_t> next{};
    return next.fetch_add(1, std::memory_order_relaxed);
}
}

World::World(std::string name)
    : entity_registry(std::make_shared<entt::registry>()), m_name(std::move(name)) {}

void World::UpdateTerrain() {
    auto cameras = Registry().view<Player, Transform>();
    for (auto entity : cameras)
        cameras.get<Player>(entity).cameraView.UpdateTerrainObserver(
            m_terrainObservers, WorldPosition(cameras.get<Transform>(entity).GetPos()));
    // Standalone CameraView components support non-player camera entities.
    auto views = Registry().view<CameraView, Transform>();
    for (auto entity : views)
        views.get<CameraView>(entity).UpdateTerrainObserver(
            m_terrainObservers, WorldPosition(views.get<Transform>(entity).GetPos()));
    m_terrainChunks.Update(m_terrainObservers.Requests());
}

Entity World::CreateEntity(std::string name) {
    const auto entity = entity_registry->create();
    return Entity{entity_registry, entity, std::move(name)};
}

const std::string& World::Name() const {
    return m_name;
}

} // namespace EESG
