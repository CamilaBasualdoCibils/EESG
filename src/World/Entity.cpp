#include "Entity.hpp"

#include <stdexcept>
#include <utility>

namespace EESG {

Entity::Entity(std::shared_ptr<entt::registry> registry, entt::entity entity, std::string name)
    : m_registry(std::move(registry)), m_entity(entity), m_name(std::move(name)) {}

const std::string& Entity::Name() const {
    static const std::string emptyName;
    return IsValid() ? m_name : emptyName;
}

bool Entity::IsValid() const {
    const auto registry = m_registry.lock();
    return registry && registry->valid(m_entity);
}

} // namespace EESG
