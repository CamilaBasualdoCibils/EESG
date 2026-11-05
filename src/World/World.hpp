#pragma once

#include <string>
#include <memory>

#include <entt/entt.hpp>

#include <Lighthouse/Engine/Scene/Entity.hpp>
#include <Lighthouse/Engine/Core/Handle.hpp>

namespace Lighthouse {


class Scene {
public:
    explicit Scene(std::string name);

    Entity CreateEntity(std::string name);
    void SetActiveCamera(const Entity& entity);
    Entity ActiveCamera() const;
    SceneHandle Handle() const noexcept { return m_handle; }

    const std::string& Name() const;

private:


    std::shared_ptr<entt::registry> m_registry;
    Entity m_activeCamera;
    std::string m_name;
    std::shared_ptr<void> m_lifetime;
    SceneHandle m_handle;
};

} // namespace Lighthouse
