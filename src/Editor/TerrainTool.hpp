#pragma once
#ifdef LIGHTHOUSE_IMGUI
#include "World/Entity/Player.hpp"
#include "World/World.hpp"
#include <Lighthouse/Engine/Tools/ISimpleTool.hpp>
#include <functional>
#include <imgui.h>
#include <unordered_map>

namespace EESG
{
using namespace Lighthouse;
class TerrainTool final : public Lighthouse::ISimpleTool
{
public:
  TerrainTool(Lighthouse::EngineRuntime&, World& world,
              std::function<void()> rebuild)
      : world_(world), rebuild_(std::move(rebuild))
  {
  }
  std::string_view Name() const noexcept override
  {
    return "Terrain";
  }
  void Draw() override
  {
    auto& terrain = world_.GetTerrain();
    bool enabled = terrain.InspectorEnabled();
    if (ImGui::Checkbox("Enable", &enabled))
    {
      terrain.EnableInspector(enabled);
      rebuild_();
    }
    ImGui::TextUnformatted("Residency only. Pixel size controls detail; "
                           "distant meta-chunks remain leaves.");
    if (ImGui::CollapsingHeader("Observer settings"))
    {
      const auto observers = world_.GetTerrainObservers().Observers();
      std::erase_if(errors_,
                    [&](const auto& error)
                    {
                      return std::none_of(observers.begin(), observers.end(),
                                          [&](auto& p)
                                          { return p.get() == error.first; });
                    });
      for (size_t i = 0; i < observers.size(); ++i)
        DrawObserver(observers[i], i);
      if (observers.empty())
        ImGui::TextUnformatted("No observers registered.");
    }
    if (!enabled)
    {
      ImGui::TextUnformatted("Enable to download residency diagnostics.");
      return;
    }
    terrain.PollReadbacks();
    if (!terrain.Diagnostic().empty())
      ImGui::TextWrapped("%s", terrain.Diagnostic().c_str());
    const auto& snapshot = terrain.Snapshot();
    if (!snapshot)
    {
      ImGui::TextUnformatted("Waiting for a completed GPU snapshot...");
      return;
    }
    const auto header = snapshot->Header();
    ImGui::Text(
        "Completed snapshot %llu | %u roots | %u nodes | %u selected leaves",
        (unsigned long long)snapshot->Revision(), header.counts.y,
        header.counts.x, header.counts.z);
    ImGui::Text("Meta-chunk levels: %u | Readbacks skipped while busy: %llu",
                TerrainMetaChunkLevels,
                (unsigned long long)terrain.SkippedReadbacks());
    if (header.counts.w)
      ImGui::TextWrapped("Node capacity reached: this snapshot is incomplete.");
    if (ImGui::BeginTable("Traversal", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
      for (auto label :
           {"Level", "Visited", "Rejected", "Subdivided", "Selected"})
        ImGui::TableSetupColumn(label);
      ImGui::TableHeadersRow();
      for (int level = int(TerrainMetaChunkLevels); level >= 0; --level)
      {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", level);
        for (int c = 0; c < 4; ++c)
        {
          ImGui::TableNextColumn();
          ImGui::Text("%u", header.levels[level][c]);
        }
      }
      ImGui::EndTable();
    }
    if (snapshot->Revision() != displayRevision_)
    {
      selected_.clear();
      for (uint32_t i = 0; i < header.counts.x; ++i)
        if (snapshot->Node(i).state.w == TerrainLeaf)
          selected_.push_back(i);
      displayRevision_ = snapshot->Revision();
    }
    if (ImGui::BeginTable("Selected leaves", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0, 280)))
    {
      for (auto label : {"Node", "Chunk origin (x,y,z)", "Meta level",
                         "Effective LOD", "Cell edge (m)", "Region edge (m)"})
        ImGui::TableSetupColumn(label);
      ImGui::TableHeadersRow();
      ImGuiListClipper clipper;
      clipper.Begin(int(selected_.size()));
      while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
          const auto index = selected_[row];
          const auto node = snapshot->Node(index);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Text("%u", index);
          ImGui::TableNextColumn();
          ImGui::Text("%d, %d, %d", node.region.x, node.region.y,
                      node.region.z);
          ImGui::TableNextColumn();
          ImGui::Text("%d", node.region.w);
          ImGui::TableNextColumn();
          ImGui::Text("%u", node.state.z);
          ImGui::TableNextColumn();
          ImGui::Text("%.3f", TerrainVoxelWidth * (1u << node.state.z));
          ImGui::TableNextColumn();
          ImGui::Text("%.1f", TerrainChunkWidth * (1u << node.region.w));
        }
      ImGui::EndTable();
    }
    // Additional stage telemetry belongs below the residency diagnostics.
  }

private:
  bool CameraFollowed(const TerrainObserverManager::Handle& observer)
  {
    auto players = world_.Registry().view<Player>();
    for (auto entity : players)
      if (players.get<Player>(entity).cameraView.GetTerrainObserver() ==
          observer)
        return true;
    auto cameras = world_.Registry().view<TerrainCamera>();
    for (auto entity : cameras)
      if (cameras.get<TerrainCamera>(entity).GetTerrainObserver() == observer)
        return true;
    return false;
  }
  void DrawObserver(const TerrainObserverManager::Handle& observer,
                    size_t index)
  {
    ImGui::PushID(observer.get());
    if (ImGui::TreeNode(("Observer " + std::to_string(index + 1)).c_str()))
    {
      auto candidate = *observer;
      bool changed = ImGui::Checkbox("Enabled", &candidate.enabled);
      int type = int(candidate.type);
      if (ImGui::Combo("Type", &type,
                       "Point (omnidirectional)\0Pyramid / frustum\0"))
      {
        candidate.type = TerrainObserverType(type);
        changed = true;
      }
      changed |=
          ImGui::InputFloat("Maximum cell size (pixels)",
                            &candidate.maxCellSizePixels, 0.5f, 2.0f, "%.2f");
      if (CameraFollowed(observer))
      {
        ImGui::TextUnformatted(
            "Position, projection and resolution follow the camera.");
        ImGui::Text("%u x %u | Vertical FOV %.1f degrees",
                    candidate.viewportSize.x, candidate.viewportSize.y,
                    candidate.verticalFovDegrees);
      }
      else
      {
        changed |= ImGui::InputScalarN("Position (m)", ImGuiDataType_Double,
                                       &candidate.position.x, 3);
        changed |= ImGui::InputScalarN("Reference viewport (pixels)",
                                       ImGuiDataType_U32,
                                       &candidate.viewportSize.x, 2);
        if (candidate.type == TerrainObserverType::Point)
          changed |= ImGui::InputFloat("Reference vertical FOV (degrees)",
                                       &candidate.verticalFovDegrees);
        if (candidate.type == TerrainObserverType::Pyramid &&
            ImGui::TreeNode("World-to-clip matrix"))
        {
          for (int c = 0; c < 4; ++c)
          {
            ImGui::PushID(c);
            changed |=
                ImGui::InputFloat4("Column", &candidate.viewProjection[c].x);
            ImGui::PopID();
          }
          ImGui::TextUnformatted("Projection * view; clip depth -1 to +1.");
          ImGui::TreePop();
        }
      }
      if (changed)
      {
        try
        {
          world_.GetTerrainObservers().Configure(observer,
                                                 std::move(candidate));
          errors_.erase(observer.get());
        }
        catch (const std::exception& e)
        {
          errors_[observer.get()] = e.what();
        }
      }
      if (auto error = errors_.find(observer.get()); error != errors_.end())
        ImGui::TextWrapped("%s", error->second.c_str());
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  World& world_;
  std::function<void()> rebuild_;
  std::unordered_map<TerrainObserver*, std::string> errors_;
  uint64_t displayRevision_{};
  std::vector<uint32_t> selected_;
};
} // namespace EESG
#endif
