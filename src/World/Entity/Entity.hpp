#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <entt/entt.hpp>
#include "EntityTypes.hpp"

namespace EESG {
class Entity {
public:
    Entity() = default;

    template <typename Component, typename... Args>
    Component& Add(Args&&... args) {
        auto registry = m_registry.lock();
        if (!registry || !registry->valid(m_entity)) {
            throw std::logic_error("Cannot add a component to an invalid entity");
        }
        return registry->emplace_or_replace<Component>(m_entity, std::forward<Args>(args)...);
    }
     template <typename Component>
     Component& Get() {
         auto registry = m_registry.lock();
         if (!registry || !registry->valid(m_entity)) {
             throw std::logic_error("Cannot get a component from an invalid entity");
         }
         return registry->get<Component>(m_entity);
     }
     template <typename Component>
     const Component& Get() const {
         auto registry = m_registry.lock();
         if (!registry || !registry->valid(m_entity)) {
             throw std::logic_error("Cannot get a component from an invalid entity");
         }
         return registry->get<Component>(m_entity);
     }
     template <typename Component>
     bool Has() const {
         auto registry = m_registry.lock();
         return registry && registry->valid(m_entity) && registry->all_of<Component>(m_entity);
     }

    const std::string& Name() const;
    bool IsValid() const;
    EntityID ID() const noexcept { return m_entity; }

private:
    friend class World;

    Entity(std::shared_ptr<entt::registry> registry, EntityID entity, std::string name);

    std::weak_ptr<entt::registry> m_registry;
    EntityID m_entity{entt::null};
};

} // namespace EESG
