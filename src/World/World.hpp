#pragma once
#include <Lighthouse/Extended/World/Scene.hpp>
#include "World/Terrain/TerrainSystem.hpp"
namespace EESG {
using namespace Lighthouse;
class World {
public:
 explicit World(std::string name) : scene_(std::move(name)) {}
 Lighthouse::Entity CreateEntity(std::string name) { return scene_.CreateEntity(std::move(name)); }
 const std::string& Name() const { return scene_.Name(); }
 entt::registry& Registry() { return scene_.Registry(); }
 Lighthouse::Scene& GetScene() { return scene_; }
 TerrainSystem& GetTerrain() { return terrain_; }
 const TerrainSystem& GetTerrain() const { return terrain_; }
 TerrainObserverManager& GetTerrainObservers() { return observers_; }
 void UpdateTerrain();
private:
 Lighthouse::Scene scene_;
 TerrainSystem terrain_;
 TerrainObserverManager observers_;
};
}
