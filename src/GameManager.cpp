#include "GameManager.hpp"
#include <Lighthouse/Backends/Input/GLFW/GLFWInputBackend.hpp>
#include "Lighthouse/Engine/Core/Application.hpp"
#include "Lighthouse/Engine/Rendering/RenderTypes.hpp"
#include "Math/Types.hpp"
#include "World/Entity/Light.hpp"
#include "World/Entity/MeshRender.hpp"
#include "World/Entity/Transform.hpp"
#include "World/WorldRenderer.hpp"
#include <Lighthouse/Engine/Assets/Assets.hpp>
#include <Lighthouse/Engine/Engine.hpp>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <limits>
#include <cmath>

#ifdef LIGHTHOUSE_IMGUI
#include <Lighthouse/Engine/Rendering/ImGuiView.hpp>
#include <Lighthouse/Engine/Tools/InputSystemTool.hpp>
#include <Lighthouse/Engine/Tools/RendererTool.hpp>
#include "Editor/EntitySystemTool.hpp"
#include "Editor/TerrainTool.hpp"
#endif

namespace EESG
{
GameManager::~GameManager()
{
#ifdef LIGHTHOUSE_IMGUI
  GetToolBox().Clear();
#endif
  GetInputSystem().RemoveActionSet(m_gameplay);
  GetInputSystem().RemoveActionSet(m_editor);
  // Release the technique while its World is still alive.
  GetRenderer().RemoveTechnique(m_worldRenderer);
  // Materials/descriptors must be released before their sampled resources.
  auto& device = *GetRenderer().GetRenderDevice();
  device.WaitIdle();
  GetRenderer().GetRenderGraph().Clear();
  m_world.Registry().clear<MeshRender>();
  m_helmetMaterials.reset();
  if (m_helmetSampler)
    device.DestroySampler(m_helmetSampler);
  for (auto view : m_helmetViews)
    device.DestroyImageView(view);
  for (auto image : m_helmetImages)
    device.DestroyImage(image);
}
void GameManager::OnStart()
{
  auto& renderer = GetRenderer();
  m_windowView = renderer.CreateWindowView(
      {.title = "EESG",
       .width = 1280,
       .height = 720,
       .decorated = true,
       .swapchainFormats = {Lighthouse::SwapchainFormat::HDR10,
                            Lighthouse::SwapchainFormat::ScRGB,
                            Lighthouse::SwapchainFormat::SRGB}});
  m_helmetAsset = Lighthouse::Assets::LoadGLTF("../DamagedHelmet.gltf");
  const auto& asset = m_helmetAsset;
  if (asset.meshes.empty() || asset.meshes[0].vertices.empty())
    throw std::runtime_error("DamagedHelmet.gltf has no mesh");
  const Lighthouse::Mesh3D& mesh3D = asset.meshes[0];
  const auto meshHandle = renderer.UploadMesh(mesh3D);
  auto dragon = m_world.CreateEntity("dragon");
  auto& transform = dragon.Add<Transform>();
  // Fit the mesh to the initial player camera, applying the glTF node's
  // quarter-turn around X. Geometry in the shared pool remains unmodified.
  Vec3 minimum(std::numeric_limits<float>::max());
  Vec3 maximum(std::numeric_limits<float>::lowest());
  for (const auto& vertex : mesh3D.vertices)
  {
    minimum = glm::min(minimum, vertex.position);
    maximum = glm::max(maximum, vertex.position);
  }
  const auto extent = maximum - minimum;
  const float scale = 2.0f / std::max({extent.x, extent.y, extent.z, 0.001f});
  const Quat rotation(glm::radians(Vec3(90.0f, 0.0f, 0.0f)));
  transform.SetRot(rotation);
  transform.SetScale(Vec3(scale));
  transform.SetPos(-(rotation * ((minimum + maximum) * (0.5f * scale))));
  auto& device = *renderer.GetRenderDevice();
  m_helmetImages.reserve(asset.images.size());
  for (const auto& image : asset.images)
    m_helmetImages.push_back(device.CreateImage(image.ToImageDesc()));
  // Explicit demo bindings from DamagedHelmet.gltf's material texture
  // indices. General glTF material import remains outside the asset loader.
  if (asset.textures.size() < 5)
    throw std::runtime_error("Helmet needs all five material textures");
  for (size_t i = 0; i < 5; ++i)
  {
    const auto imageIndex = asset.textures[i].image;
    if (imageIndex >= m_helmetImages.size())
      throw std::runtime_error("Invalid helmet texture source");
    m_helmetViews.push_back(device.CreateImageView(
        {.image = m_helmetImages[imageIndex],
         .format = asset.images[imageIndex].GetFormat()}));
  }
  m_helmetSampler = device.CreateSampler(
      {.minification = Lighthouse::SamplerFilter::Linear,
       .magnification = Lighthouse::SamplerFilter::Linear,
       .addressU = Lighthouse::SamplerAddressMode::Repeat,
       .addressV = Lighthouse::SamplerAddressMode::Repeat});
  m_helmetMaterials = std::make_unique<Materials>(device);
  MaterialLoadOptions options;
  options.textures["albedo"] = {m_helmetViews[0], m_helmetSampler};
  options.textures["metallicRoughness"] = {m_helmetViews[1], m_helmetSampler};
  options.textures["emissive"] = {m_helmetViews[2], m_helmetSampler};
  options.textures["occlusion"] = {m_helmetViews[3], m_helmetSampler};
  options.textures["normalMap"] = {m_helmetViews[4], m_helmetSampler};
  auto material = m_helmetMaterials->Load("materials/Helmet.material.json")
                      ->Instantiate(options);
  dragon.Add<MeshRender>(meshHandle, std::move(material));
  auto overhead = m_world.CreateEntity("Overhead light");
  overhead.Add<Transform>().SetRot(glm::quatLookAtRH(
      -glm::normalize(Vec3(-0.25f, 1.0f, 0.45f)), Vec3(0, 0, 1)));
  overhead.Add<Light>(Light{.type = LightType::Directional, .intensity = 3.0f});

  auto leftPointLight = m_world.CreateEntity("Left point light");
  leftPointLight.Add<Transform>().SetPos({-2.0f, 0, 0.0f});
  leftPointLight.Add<Light>(
      Light{.type = LightType::Point, .intensity = 2.0f, .range = 5.0f});
  // Three blocky spheres in view of the player camera, spanning chunk
  // boundaries so the demo also exercises pooled chunk draws.
  for (const auto center : {Vec3(-1.05f, -0.65f, 0.3f),
                            Vec3(0.95f, -0.6f, 0.1f), Vec3(0.0f, 0.9f, -0.5f)})
  {
    constexpr float radius = 0.45f;
    const auto low = WorldTerrain::voxel_position(Dvec3(center - Vec3(radius)));
    const auto high =
        WorldTerrain::voxel_position(Dvec3(center + Vec3(radius)));
    for (auto z = low[2]; z <= high[2]; ++z)
      for (auto y = low[1]; y <= high[1]; ++y)
        for (auto x = low[0]; x <= high[0]; ++x)
        {
          const Vec3 point =
              (Vec3(x, y, z) + Vec3(0.5f)) * float(WorldTerrain::voxel_width);
          if (glm::dot(point - center, point - center) <= radius * radius)
            m_world.GetTerrain().set_voxel({x, y, z}, {1, 1});
        }
  }
  auto player = m_world.CreateEntity("Player");
  player.Add<Transform>().SetPos({0, 0, 3.6f});
  player.Add<Player>();
  m_player = player;
  auto& input = GetInputSystem();
  auto& backend = input.GetBackend<Lighthouse::GLFWInputBackend>();
  const auto keyboards = backend.EnumerateKeyboards();
  if (keyboards.empty()) throw std::runtime_error("No keyboard available");
  auto keyboard = backend.OpenDevice(keyboards.front().id,
      renderer.GetView<Lighthouse::WindowView>(m_windowView)->GetWindow()->Handle());
  m_gameplay = input.CreateActionSet("Gameplay");
  m_move = m_gameplay->AddAction("Move", Lighthouse::InputActionType::Axis2D);
  using Lighthouse::Key;
  m_move->BindComposite2D(keyboard, Key::A, Key::D, Key::S, Key::W);
  m_elevate = m_gameplay->AddAction("Elevate", Lighthouse::InputActionType::Axis1D);
  using Control = Lighthouse::InputControl;
  m_elevate->Bind(keyboard, Control::Button(Key::Space), {1, 0}); // Space
  m_elevate->Bind(keyboard, Control::Button(Key::LeftShift), {-1, 0}); // Left Shift
  m_elevate->Bind(keyboard, Control::Button(Key::RightShift), {-1, 0}); // Right Shift
  const auto window = renderer.GetView<Lighthouse::WindowView>(m_windowView)->GetWindow()->Handle();
  const auto mice = backend.EnumerateMice();
  if (mice.empty()) throw std::runtime_error("No mouse available");
  auto mouse = backend.OpenDevice(mice.front().id, window);
  m_look = m_gameplay->AddAction("Look", Lighthouse::InputActionType::Delta2D);
  m_look->Bind(mouse, Control::Delta(Lighthouse::MouseAxis::X), {1, 0});
  m_look->Bind(mouse, Control::Delta(Lighthouse::MouseAxis::Y), {0, 1});
  GetWindowManager().SetCursorMode(window, Lighthouse::CursorMode::Captured);
  m_editor = input.CreateActionSet("Editor");
  auto toggleCursor = m_editor->AddAction("ToggleCursor", Lighthouse::InputActionType::Button);
  toggleCursor->Bind(keyboard, Control::Button(Key::Escape));
  toggleCursor->OnPressed([this, window] {
    m_cursorCaptured = !m_cursorCaptured;
    GetWindowManager().SetCursorMode(window, m_cursorCaptured ? Lighthouse::CursorMode::Captured : Lighthouse::CursorMode::Normal);
  });
  m_editor->SetEnabled(true);
  m_gameplay->SetEnabled(true);
  m_worldRenderer =
      renderer.AddTechnique<WorldRenderer>(&renderer, &m_world, m_windowView)
          .handle;
#ifdef LIGHTHOUSE_IMGUI
  GetToolBox().AddTool<Lighthouse::InputSystemTool>();
  GetToolBox().AddTool<TerrainTool>(m_world, [this] {
    return static_cast<WorldRenderer*>(GetRenderer().GetTechnique(m_worldRenderer))->TerrainStats();
  },[this](size_t count) {
    static_cast<WorldRenderer*>(GetRenderer().GetTechnique(m_worldRenderer))->SetTerrainMeshingThreadCount(count);
  });
  GetToolBox().AddTool<EntitySystemTool>(m_world, [this] { m_recompile = true; });
  GetToolBox().AddTool<Lighthouse::RendererTool>();
  const auto imgui = renderer.CreateView<Lighthouse::ImGuiView>();
  renderer.GetView<Lighthouse::ImGuiView>(imgui)->AddObject(GetToolBox());
#endif
  const auto size = renderer.GetView<Lighthouse::WindowView>(m_windowView)
                        ->GetWindow()
                        ->GetFramebufferSize();
  m_width = size.width;
  m_height = size.height;
  m_world.UpdateTerrain();
  static_cast<WorldRenderer*>(renderer.GetTechnique(m_worldRenderer))->UpdateTerrainMeshes();
  renderer.Compile();
  std::cout << renderer.GetRenderGraph().Dot() << '\n';
}
void GameManager::OnUpdate(float deltaTime)
{
  auto& renderer = GetRenderer();
  const auto size = renderer.GetView<Lighthouse::WindowView>(m_windowView)
                        ->GetWindow()
                        ->GetFramebufferSize();
  if (!size.width || !size.height)
    return;
  auto* window = renderer.GetView<Lighthouse::WindowView>(m_windowView)->GetWindow();
  const bool acceptInput = m_cursorCaptured && window->IsFocused() && m_gameplay->IsEnabled();
  m_gameplay->SetEnabled(m_cursorCaptured && window->IsFocused());
  auto& transform = m_player.Get<Transform>();
  if (acceptInput) {
    const auto look = m_look->GetAxis2D();
    constexpr float sensitivity = 0.002f; // Radians per cursor unit.
    const Vec3 direction = transform.GetRot() * Vec3(0, 0, -1);
    m_cameraYaw = std::atan2(-direction.x, -direction.z);
    m_cameraPitch = std::asin(std::clamp(direction.y, -1.f, 1.f));
    m_cameraYaw = std::remainder(m_cameraYaw - look.x * sensitivity, glm::two_pi<float>());
    m_cameraPitch = std::clamp(m_cameraPitch - look.y * sensitivity,
                               -glm::radians(89.f), glm::radians(89.f));
    transform.SetRot(glm::angleAxis(m_cameraYaw, Vec3(0, 1, 0)) *
                     glm::angleAxis(m_cameraPitch, Vec3(1, 0, 0)));
  }
  const auto move = acceptInput ? m_move->GetAxis2D() : glm::vec2(0);
  Vec3 forward = transform.GetRot() * Vec3(0, 0, -1);
  forward.y = 0;
  const float forwardLength = glm::length(forward);
  forward = forwardLength > 0.0001f ? forward / forwardLength : Vec3(0, 0, -1);
  const Vec3 right = glm::cross(forward, Vec3(0, 1, 0));
  Vec3 movement = right * move.x + forward * move.y + Vec3(0, acceptInput ? m_elevate->GetAxis1D() : 0.f, 0);
  const float length = glm::length(movement);
  if (length > 1) movement /= length;
  constexpr float flySpeed = 3.0f;
  transform.SetPos(transform.GetPos() + movement * (flySpeed * deltaTime));
  static_cast<WorldRenderer*>(renderer.GetTechnique(m_worldRenderer))
      ->SetDeltaTime(deltaTime);
  m_world.UpdateTerrain();
  static_cast<WorldRenderer*>(renderer.GetTechnique(m_worldRenderer))->UpdateTerrainMeshes();
  if (m_recompile ||
      size.width != m_width || size.height != m_height)
  {
    renderer.Compile();
    m_recompile = false;
    m_width = size.width;
    m_height = size.height;
  }
  renderer.Render();
}
} // namespace EESG
