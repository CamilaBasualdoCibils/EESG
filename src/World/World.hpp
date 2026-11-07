#pragma once

#include "World/Entity/Entity.hpp"
#include "World/Terrain/WorldTerrain.hpp"
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
    WorldTerrain& GetTerrain() { return m_terrain; }
    const WorldTerrain& GetTerrain() const { return m_terrain; }
private:


    std::shared_ptr<entt::registry> entity_registry;
    std::string m_name;
    WorldTerrain m_terrain;


};

} // namespace EESG
