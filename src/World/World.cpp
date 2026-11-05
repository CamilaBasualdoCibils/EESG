#include "World/World.hpp"

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

Entity World::CreateEntity(std::string name) {
    const auto entity = entity_registry->create();
    return Entity{entity_registry, entity, std::move(name)};
}

const std::string& World::Name() const {
    return m_name;
}

} // namespace EESG
