#pragma once

#include "World/Entity/Entity.hpp"
#include "World/Terrain/TerrainObserverManager.hpp"
#include "World/Terrain/TerrainSystem.hpp"
#include <memory>
#include <string>

#include <entt/entt.hpp>

namespace EESG {


class World {
public:
    explicit World(std::string name);

    Entity CreateEntity(std::string name);

    const std::string& Name() const;

    entt::registry& Registry() { return *entity_registry; }
    TerrainSystem& GetTerrain()
    {
      return m_terrain;
    }
    const TerrainSystem& GetTerrain() const
    {
      return m_terrain;
    }
    TerrainObserverManager& GetTerrainObservers() { return m_terrainObservers; }
    void UpdateTerrain();
private:


    std::shared_ptr<entt::registry> entity_registry;
    std::string m_name;
    TerrainSystem m_terrain;
    TerrainObserverManager m_terrainObservers;


};

} // namespace EESG
