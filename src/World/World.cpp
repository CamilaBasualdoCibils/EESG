#include <Lighthouse/Engine/Scene/Scene.hpp>

#include <stdexcept>
#include <atomic>
#include <utility>

namespace Lighthouse {
namespace {
std::uint32_t NextSceneIndex() {
    static std::atomic<std::uint32_t> next{};
    return next.fetch_add(1, std::memory_order_relaxed);
}
}

Scene::Scene(std::string name)
    : m_registry(std::make_shared<entt::registry>()), m_name(std::move(name)),
      m_lifetime(std::make_shared<std::uint8_t>(0)), m_handle(NextSceneIndex(), 1, m_lifetime) {}

Entity Scene::CreateEntity(std::string name) {
    const auto entity = m_registry->create();
    return Entity{m_registry, entity, std::move(name)};
}

void Scene::SetActiveCamera(const Entity& entity) {
    if (!entity.IsValid() || entity.m_registry.lock() != m_registry) {
        throw std::invalid_argument("Active camera must be a valid entity in this scene");
    }

    m_activeCamera = entity;
}

Entity Scene::ActiveCamera() const {
    return m_activeCamera.IsValid() ? m_activeCamera : Entity{};
}

const std::string& Scene::Name() const {
    return m_name;
}

} // namespace Lighthouse
