#pragma once

#include "World/Entity/Entity.hpp"
#include "World/Terrain/PerlinTerrainGenerator.hpp"
#include "World/Terrain/TerrainObserverManager.hpp"
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
    TerrainChunkManager& GetTerrainChunks() { return m_terrainChunks; }
    TerrainObserverManager& GetTerrainObservers() { return m_terrainObservers; }
    void UpdateTerrain();
private:


    std::shared_ptr<entt::registry> entity_registry;
    std::string m_name;
    WorldTerrain m_terrain;
    PerlinTerrainGenerator m_terrainGenerator;
    NullTerrainStorage m_terrainStorage;
    TerrainChunkManager m_terrainChunks{m_terrain, m_terrainGenerator, m_terrainStorage};
    TerrainObserverManager m_terrainObservers;


};

} // namespace EESG
