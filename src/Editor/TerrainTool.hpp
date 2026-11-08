#pragma once
#ifdef LIGHTHOUSE_IMGUI
#include "World/Entity/Player.hpp"
#include "World/Terrain/TerrainMeshPool.hpp"
#include "World/World.hpp"
#include <Lighthouse/Engine/Tools/ISimpleTool.hpp>
#include <algorithm>
#include <functional>
#include <imgui.h>
#include <tuple>

namespace EESG
{
class TerrainTool final : public Lighthouse::ISimpleTool
{
public:
  TerrainTool(Lighthouse::EngineRuntime&, World& world,
              std::function<TerrainMeshStats()> meshStats = {},
              std::function<void(size_t)> setMeshingThreadCount = {})
      : world_(world), meshStats_(std::move(meshStats)),
        setMeshingThreadCount_(std::move(setMeshingThreadCount))
  {
  }
  std::string_view Name() const noexcept override
  {
    return "Terrain";
  }
  void Draw() override
  {
    auto& terrain = world_.GetTerrain();
    const auto& resident = world_.GetTerrainChunks().Resident();
    ImGui::Text("World: %s", world_.Name().c_str());
    ImGui::Text("Sources: %zu | Loaded views: %zu | Resident: %zu",
                terrain.source_chunk_count(), terrain.loaded_region_count(),
                resident.size());
    auto& chunks = world_.GetTerrainChunks();
    ImGui::Text("CPU octree buffers: %.2f / %.2f MiB (peak %.2f)",
                chunks.MemoryUsed() / 1048576.0,
                chunks.MemoryLimit() / 1048576.0,
                chunks.MemoryPeak() / 1048576.0);
    int limitMiB = int(chunks.MemoryLimit() / 1048576);
    if (ImGui::InputInt("CPU budget (MiB)", &limitMiB) && limitMiB >= 0)
      chunks.SetMemoryLimit(size_t(limitMiB) * 1048576);
    ImGui::TextUnformatted(
        "Storage: none (regenerated each run). LOD 0 is finest.");
    if (ImGui::CollapsingHeader("Observer settings",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
      const auto observers = world_.GetTerrainObservers().Observers();
      std::erase_if(observerErrors_,
                    [&](const auto& error)
                    {
                      return std::none_of(
                          observers.begin(), observers.end(),
                          [&](const auto& observer)
                          { return observer.get() == error.first; });
                    });
      if (observers.empty())
        ImGui::TextUnformatted("No terrain observers registered.");
      for (size_t i = 0; i < observers.size(); ++i)
        DrawObserver(observers[i], i);
    }
    if (meshStats_)
    {
      const auto stats = meshStats_();
      DrawThreads(stats);
      ImGui::SeparatorText("Meshing");
      ImGui::Text("Pending: %zu | Drawn chunks: %zu | Jobs scheduled: %llu",
                  stats.pending, stats.chunks,
                  (unsigned long long)stats.scheduled);
      ImGui::Text("Shared GPU buffer: %.2f / %.2f MiB",
                  stats.bufferUsed / 1048576.0,
                  stats.bufferCapacity / 1048576.0);
      ImGui::Text("Uploaded this frame: %.1f KiB (%zu chunks)",
                  stats.uploadedBytes / 1024.0, stats.uploadedChunks);
      ImGui::Text("Stale results discarded: %llu",
                  (unsigned long long)stats.discarded);
      if (!stats.error.empty())
        ImGui::TextWrapped("%s", stats.error.c_str());
    }
    ImGui::TextUnformatted(
        "LOD 0 = 0.125 m (8 voxels/m), LOD 1 = 0.250 m, LOD 2 = 0.500 m.");
    ImGui::TextWrapped("Chunks stay 32 m across. Resident LOD can be coarser "
                       "than requested when the CPU budget is reached. Merged "
                       "coplanar faces can span multiple voxels.");
    ImGui::SeparatorText("Chunks");
    auto keys = terrain.source_keys();
    // Include resident empty/air chunks, which have no occupied octree leaves.
    for (const auto& [key, lod] : resident)
      if (std::find(keys.begin(), keys.end(), key) == keys.end())
        keys.push_back(key);
    std::sort(keys.begin(), keys.end(), [](auto a, auto b)
              { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); });
    if (ImGui::BeginTable("chunks", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0, 220)))
    {
      for (auto label : {"Chunk", "Requested LOD", "Resident LOD",
                         "Voxel edge (m)", "Leaves", "Inspect"})
        ImGui::TableSetupColumn(label);
      ImGui::TableHeadersRow();
      const auto requests = world_.GetTerrainObservers().Requests();
      for (size_t i = 0; i < keys.size(); ++i)
      {
        const auto key = keys[i];
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%lld, %lld, %lld", (long long)key.x, (long long)key.y,
                    (long long)key.z);
        ImGui::TableNextColumn();
        if (auto it = requests.find(key); it != requests.end())
          ImGui::Text("%u", it->second.lod);
        else
          ImGui::TextUnformatted("-");
        ImGui::TableNextColumn();
        if (auto it = resident.find(key); it != resident.end())
          ImGui::Text("%u", it->second);
        else
          ImGui::TextUnformatted("Edited");
        ImGui::TableNextColumn();
        const auto residentChunk = resident.find(key);
        const auto actualLOD =
            residentChunk == resident.end() ? 0u : residentChunk->second;
        ImGui::Text("%.3f", WorldTerrain::voxel_width * (1u << actualLOD));
        if (residentChunk != resident.end() &&
            terrain.source_lod(key) < actualLOD && ImGui::IsItemHovered())
          ImGui::SetTooltip("Procedural voxels are %.3f m; this chunk also "
                            "contains finer authored edits.",
                            WorldTerrain::voxel_width * (1u << actualLOD));
        ImGui::TableNextColumn();
        const auto* tree = terrain.source(key);
        ImGui::Text("%zu", tree ? tree->terminal_count() : 0);
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Inspect"))
          selected_ = key;
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (!selected_)
      return;
    const auto key = *selected_;
    const auto origin = WorldTerrain::origin(key);
    ImGui::Text("Selected: %lld, %lld, %lld", (long long)key.x,
                (long long)key.y, (long long)key.z);
    ImGui::Text("Origin (m): %.2f, %.2f, %.2f",
                origin[0] * WorldTerrain::voxel_width,
                origin[1] * WorldTerrain::voxel_width,
                origin[2] * WorldTerrain::voxel_width);
    ImGui::SliderInt3("Local voxel", local_, 0, 255);
    const auto value = terrain.sample_voxel(
        {origin[0] + local_[0], origin[1] + local_[1], origin[2] + local_[2]});
    ImGui::Text("Material: %u | Occupancy: %.3f", value.material,
                value.density);
    if (const auto* tree = terrain.source(key);
        tree && ImGui::CollapsingHeader("Octree leaves (first 128)"))
    {
      size_t count = 0;
      tree->visit(
          [&](const TerrainRegion::Tree::Entry& entry)
          {
            if (!entry.terminal || count++ >= 128)
              return;
            const auto voxel = tree->cell(entry.handle);
            ImGui::Text("[%u,%u,%u] depth %u, material %u, density %.3f",
                        entry.region.origin[0], entry.region.origin[1],
                        entry.region.origin[2], entry.region.depth,
                        voxel.material, voxel.density);
          });
    }
  }

private:
  CameraView* CameraFor(const TerrainObserverManager::Handle& observer)
  {
    auto players = world_.Registry().view<Player>();
    for (auto entity : players)
    {
      auto& camera = players.get<Player>(entity).cameraView;
      if (camera.GetTerrainObserver() == observer)
        return &camera;
    }
    auto cameras = world_.Registry().view<CameraView>();
    for (auto entity : cameras)
      if (cameras.get<CameraView>(entity).GetTerrainObserver() == observer)
        return &cameras.get<CameraView>(entity);
    return nullptr;
  }
  void DrawObserver(const TerrainObserverManager::Handle& observer,
                    size_t index)
  {
    ImGui::PushID(observer.get());
    const auto label = "Observer " + std::to_string(index + 1);
    if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
      auto candidate = *observer;
      auto* camera = CameraFor(observer);
      bool changed = ImGui::Checkbox("Enabled", &candidate.enabled);
      changed |= ImGui::InputInt("Priority", &candidate.priority);
      if (camera)
      {
        ImGui::Text("Camera-followed position: %.2f, %.2f, %.2f",
                    candidate.position.x, candidate.position.y,
                    candidate.position.z);
      }
      else
        changed |= ImGui::InputScalarN("Position (m)", ImGuiDataType_Double,
                                       &candidate.position.x, 3, nullptr,
                                       nullptr, "%.3f");
      for (size_t i = 0; i < candidate.lodLevels.size(); ++i)
      {
        ImGui::PushID(int(i));
        auto& band = candidate.lodLevels[i];
        changed |=
            ImGui::InputDouble("Distance (m)", &band.distance, 1, 10, "%.1f");
        int lod = int(band.lod);
        const int minimum = i ? int(candidate.lodLevels[i - 1].lod) : 0;
        const int maximum = i + 1 < candidate.lodLevels.size()
                                ? int(candidate.lodLevels[i + 1].lod)
                                : int(WorldTerrain::chunk_depth);
        if (ImGui::SliderInt("LOD", &lod, minimum, maximum))
        {
          band.lod = uint32_t(lod);
          changed = true;
        }
        ImGui::Text("Voxel edge: %.3f m (%u finest voxels)",
                    WorldTerrain::voxel_width * (1u << band.lod),
                    1u << band.lod);
        const bool remove = ImGui::SmallButton("Remove LOD band");
        ImGui::PopID();
        if (remove)
        {
          candidate.lodLevels.erase(candidate.lodLevels.begin() + i);
          changed = true;
          break;
        }
        ImGui::Separator();
      }
      if (ImGui::SmallButton("Add LOD band"))
      {
        candidate.lodLevels.push_back(
            candidate.lodLevels.empty()
                ? TerrainLODLevel{32, 0}
                : TerrainLODLevel{
                      std::max(candidate.lodLevels.back().distance * 2,
                               candidate.lodLevels.back().distance + 32),
                      std::min(candidate.lodLevels.back().lod + 1,
                               WorldTerrain::chunk_depth)});
        changed = true;
      }
      if (changed)
      {
        try
        {
          world_.GetTerrainObservers().Configure(observer,
                                                 std::move(candidate));
          if (camera)
            camera->terrainPriority = observer->priority;
          observerErrors_.erase(observer.get());
        }
        catch (const std::exception& error)
        {
          observerErrors_[observer.get()] = error.what();
        }
      }
      if (const auto error = observerErrors_.find(observer.get());
          error != observerErrors_.end())
        ImGui::TextWrapped("Settings not applied: %s", error->second.c_str());
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  void DrawBackgroundWorker(const char* label,
                            const TerrainBackgroundStatus& worker)
  {
    ImGui::Text("%s: %s | Thread %s | %.2f ms", label, worker.state.c_str(),
                worker.threadId.empty() ? "-" : worker.threadId.c_str(),
                worker.milliseconds);
    if (worker.chunk)
      ImGui::Text("Chunk %lld, %lld, %lld | LOD %u", (long long)worker.chunk->x,
                  (long long)worker.chunk->y, (long long)worker.chunk->z,
                  worker.lod);
    if (!worker.lastResult.empty())
      ImGui::TextWrapped("Last result: %s", worker.lastResult.c_str());
  }
  void DrawThreads(TerrainMeshStats stats)
  {
    ImGui::SeparatorText("Terrain threads");
    auto& chunks = world_.GetTerrainChunks();
    int count = int(chunks.GenerationThreadCount());
    if (ImGui::InputInt("Generation threads (1-64)", &count))
      chunks.SetGenerationThreadCount(size_t(std::clamp(
          count, 1, int(TerrainChunkManager::maximum_generation_threads))));
    const auto generation = chunks.GenerationStats();
    ImGui::Text("Generation: %zu active / %zu configured | %zu jobs in flight",
                generation.activeWorkers, generation.threadCount,
                generation.pending);
    if (generation.retiringWorkers)
      ImGui::Text("Retiring workers: %zu (draining without blocking rendering)",
                  generation.retiringWorkers);
    if (generation.memoryPressure)
      ImGui::TextWrapped("CPU budget pressure: generation allocations are "
                         "serialized in observer-priority order.");
    for (size_t i = 0; i < generation.workers.size(); ++i)
    {
      const auto label = "Generation worker " + std::to_string(i + 1) +
                         (i >= generation.threadCount ? " (retiring)" : "");
      DrawBackgroundWorker(label.c_str(), generation.workers[i]);
    }
    ImGui::Text(
        "Completed this update: %zu generated, %zu loaded, %zu budget-blocked",
        generation.generated, generation.loaded, generation.budgetBlocked);
    if (generation.budgetPending)
      ImGui::TextWrapped(
          "CPU budget change pending background retirement; existing "
          "allocations are draining without blocking rendering.");
    if (!generation.error.empty())
      ImGui::TextWrapped("Generation error: %s", generation.error.c_str());
    ImGui::TextUnformatted("Uploading: 1 background copy worker; GPU "
                           "completion is polled without waiting");
    DrawBackgroundWorker("Upload worker", stats.uploadWorker);
    ImGui::Text("GPU ranges awaiting safe reuse: %zu", stats.retiringRanges);
    if (setMeshingThreadCount_)
    {
      int count = int(stats.workerLimit);
      if (ImGui::InputInt("Meshing threads (1-64)", &count))
      {
        setMeshingThreadCount_(size_t(std::clamp(
            count, 1, int(TerrainMeshScheduler::maximum_meshing_threads))));
        if (meshStats_)
          stats = meshStats_();
      }
    }
    if (stats.retiringWorkers)
      ImGui::Text("Retiring meshing workers: %zu (finishing in background)",
                  stats.retiringWorkers);
    ImGui::Text("Meshing: %zu active threads / %zu configured",
                stats.activeWorkers, stats.workerLimit);
    ImGui::TextWrapped("Jobs launch background threads as needed. Idle rows "
                       "retain the last thread and task.");
    if (ImGui::BeginTable("Terrain workers", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
      for (auto label : {"Worker", "Thread ID", "State", "Current / last chunk",
                         "LOD", "Time (ms)"})
        ImGui::TableSetupColumn(label);
      ImGui::TableHeadersRow();
      for (const auto& worker : stats.workers)
      {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%zu%s", worker.slot, worker.retiring ? " (retiring)" : "");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(
            worker.threadId.empty() ? "-" : worker.threadId.c_str());
        ImGui::TableNextColumn();
        const char* state =
            worker.phase == TerrainWorkerPhase::Idle       ? "Idle"
            : worker.phase == TerrainWorkerPhase::Starting ? "Starting"
            : worker.phase == TerrainWorkerPhase::Meshing
                ? (worker.lastResult == "Releasing obsolete CPU meshes"
                       ? "Releasing CPU meshes"
                       : "Meshing voxels")
                : "Finished; awaiting pickup";
        ImGui::TextUnformatted(state);
        if (!worker.lastResult.empty() && ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", worker.lastResult.c_str());
        ImGui::TableNextColumn();
        if (worker.chunk)
          ImGui::Text("%lld, %lld, %lld", (long long)worker.chunk->x,
                      (long long)worker.chunk->y, (long long)worker.chunk->z);
        else
          ImGui::TextUnformatted("-");
        ImGui::TableNextColumn();
        if (worker.chunk)
          ImGui::Text("%u", worker.lod);
        else
          ImGui::TextUnformatted("-");
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", worker.milliseconds);
      }
      ImGui::EndTable();
    }
  }
  std::unordered_map<TerrainObserver*, std::string> observerErrors_;
  World& world_;
  std::function<TerrainMeshStats()> meshStats_;
  std::function<void(size_t)> setMeshingThreadCount_;
  std::optional<RegionKey> selected_;
  int local_[3]{};
};
} // namespace EESG
#endif
