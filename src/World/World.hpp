#pragma once

#include "World/Entity.hpp"
#include <string>
#include <memory>

#include <entt/entt.hpp>

namespace EESG {


class World {
public:
    explicit World(std::string name);

    Entity CreateEntity(std::string name);

    const std::string& Name() const;

private:


    std::shared_ptr<entt::registry> m_registry;
    std::string m_name;

};

} // namespace EESG
