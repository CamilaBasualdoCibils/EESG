#include "Entity.hpp"

#include <stdexcept>
#include <utility>

namespace EESG {

Entity::Entity(std::shared_ptr<entt::registry> registry, entt::entity entity, std::string name)
    : m_registry(registry), m_entity(entity) {
    registry->emplace_or_replace<EntityName>(entity, std::move(name));
}

const std::string& Entity::Name() const {
    static const std::string emptyName;
    const auto registry = m_registry.lock();
    if (!registry || !registry->valid(m_entity)) return emptyName;
    const auto* name = registry->try_get<EntityName>(m_entity);
    return name ? name->value : emptyName;
}

bool Entity::IsValid() const {
    const auto registry = m_registry.lock();
    return registry && registry->valid(m_entity);
}

} // namespace EESG
