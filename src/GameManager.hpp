#pragma once
#include "World/Entity/Player.hpp"
#include "World/WorldRenderer.hpp"
#include <Lighthouse/Engine/Assets/Assets.hpp>
#include <Lighthouse/Engine/Core/Application.hpp>
namespace EESG
{
class GameManager final : public Lighthouse::IApplication
{
public:
  ~GameManager() override;
  void OnStart() override;
  void OnUpdate(float deltaTime) override;

private:
  std::shared_ptr<Lighthouse::InputActionSet> m_gameplay;
  std::shared_ptr<Lighthouse::InputAction> m_move, m_elevate, m_look;
  float m_cameraYaw{}, m_cameraPitch{};
  Lighthouse::GLTFAsset m_helmetAsset;
  std::vector<Lighthouse::ImageHandle> m_helmetImages;
  std::vector<Lighthouse::ImageViewHandle> m_helmetViews;
  Lighthouse::SamplerHandle m_helmetSampler{};
  std::unique_ptr<Materials> m_helmetMaterials;
  Entity m_player;
  World m_world{"MainWorld"};
  Lighthouse::RenderViewHandle m_windowView{};
  Lighthouse::RenderTechniqueHandle m_worldRenderer{};
  std::uint32_t m_width{}, m_height{};
  uint64_t m_terrainRevision{};
  bool m_recompile{};
};
} // namespace EESG
