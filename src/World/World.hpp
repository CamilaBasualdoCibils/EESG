#pragma once

#include "World/Entity/Entity.hpp"
#include "World/Terrain/Terrain.hpp"
#include <string>
#include <memory>

#include <entt/entt.hpp>

namespace EESG {


class World {
public:
    explicit World(std::string name);

    Entity CreateEntity(std::string name);

    const std::string& Name() const;

    entt::registry& Registry() { return *entity_registry; }
    Terrain& GetTerrain() { return *m_terrain; }
private:


    std::shared_ptr<entt::registry> entity_registry;
    std::string m_name;
    std::unique_ptr<Terrain> m_terrain;


};

} // namespace EESG
