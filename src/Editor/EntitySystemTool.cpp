#ifdef LIGHTHOUSE_IMGUI
#include "EntitySystemTool.hpp"
#include "World/Entity/Transform.hpp"
#include "World/World.hpp"
#include <algorithm>
#include <imgui.h>
#include <unordered_set>
namespace EESG
{
void EntitySystemTool::Draw()
{
  bool rebuild = false;
  auto& registry = m_world.Registry();
  if (m_selected != entt::null && !registry.valid(m_selected))
    m_selected = entt::null;
  if (ImGui::BeginTable("EntityPanels", 2,
                        ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_BordersInnerV))
  {
    ImGui::TableSetupColumn("Hierarchy", ImGuiTableColumnFlags_WidthFixed, 220);
    ImGui::TableSetupColumn("Inspector");
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::BeginChild("Hierarchy");
    std::vector<entt::entity> entities;
    for (auto entity : registry.storage<entt::entity>())
      entities.push_back(entity);
    std::unordered_set<entt::entity> visited;
    auto parentOf = [&](entt::entity entity)
    {
      auto* transform = registry.try_get<Transform>(entity);
      return transform && transform->GetParent() ? *transform->GetParent()
                                                 : entt::null;
    };
    std::function<void(entt::entity)> drawEntity = [&](entt::entity entity)
    {
      if (!visited.insert(entity).second)
        return;
      ImGui::PushID(int(entt::to_integral(entity)));
      const auto* name = registry.try_get<EntityName>(entity);
      bool children = std::any_of(
          entities.begin(), entities.end(), [&](auto child)
          { return parentOf(child) == entity && !visited.contains(child); });
      auto flags =
          ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
      if (!children)
        flags |= ImGuiTreeNodeFlags_Leaf;
      if (m_selected == entity)
        flags |= ImGuiTreeNodeFlags_Selected;
      bool open = ImGui::TreeNodeEx(
          "Entity", flags, "%s", name ? name->value.c_str() : "Unnamed entity");
      if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        m_selected = entity;
      if (open)
      {
        for (auto child : entities)
          if (parentOf(child) == entity)
            drawEntity(child);
        ImGui::TreePop();
      }
      else
      {
        // Mark descendants so collapsed children are not drawn as roots.
        std::function<void(entt::entity)> mark = [&](auto current)
        {
          for (auto child : entities)
            if (parentOf(child) == current && visited.insert(child).second)
              mark(child);
        };
        mark(entity);
      }
      ImGui::PopID();
    };
    for (auto entity : entities)
      if (!registry.valid(parentOf(entity)))
        drawEntity(entity);
    for (auto entity : entities)
      if (!visited.contains(entity))
        drawEntity(entity); // Cycles remain inspectable.
    ImGui::EndChild();
    ImGui::TableNextColumn();
    ImGui::BeginChild("Inspector");
    if (m_selected == entt::null)
      ImGui::TextUnformatted("Select an entity.");
    else
      for (auto [id, storage] : registry.storage())
      {
        if (!storage.contains(m_selected))
          continue;
        auto type = entt::resolve(storage.info());
        const ComponentInspectorInfo* info =
            type ? type.custom() : entt::meta_custom{};
        ImGui::PushID(int(id));
        if (ImGui::CollapsingHeader(info ? info->name
                                         : storage.info().name().data(),
                                    ImGuiTreeNodeFlags_DefaultOpen))
        {
          if (auto inspect = type.func(InspectEntityID))
          {
            auto result =
                inspect.invoke({}, entt::forward_as_meta(registry), m_selected);
            if (auto* changed = result.try_cast<bool>())
              rebuild |= *changed;
          }
          else
            ImGui::TextUnformatted("No component inspector registered.");
        }
        ImGui::PopID();
      }
    ImGui::EndChild();
    ImGui::EndTable();
  }
  if (rebuild && m_onRebuild)
    m_onRebuild();
}
} // namespace EESG
#endif
