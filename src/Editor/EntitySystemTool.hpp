#pragma once
#ifdef LIGHTHOUSE_IMGUI
#include "World/Entity/EntityTypes.hpp"
#include <Lighthouse/Engine/Tools/ISimpleTool.hpp>
#include <functional>
namespace EESG
{
class World;
class EntitySystemTool final : public Lighthouse::ISimpleTool
{
public:
  EntitySystemTool(Lighthouse::EngineRuntime&, World& world,
                   std::function<void()> onRebuild)
      : m_world(world), m_onRebuild(std::move(onRebuild))
  {
  }
  std::string_view Name() const noexcept override
  {
    return "Entities";
  }
  void Draw() override;

private:
  World& m_world;
  std::function<void()> m_onRebuild;
  entt::entity m_selected{entt::null};
};
} // namespace EESG
#endif
