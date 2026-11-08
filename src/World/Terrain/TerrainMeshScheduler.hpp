#pragma once
#include "World/Terrain/TerrainMesher.hpp"
#include "World/Terrain/TerrainThreadInfo.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace EESG
{
struct TerrainMeshStamp
{
  std::array<uint64_t, 7> sources{};
  uint32_t lod = 0;
  bool operator==(const TerrainMeshStamp&) const = default;
  static TerrainMeshStamp Read(const WorldTerrain& terrain, RegionKey key)
  {
    TerrainMeshStamp stamp;
    stamp.lod = terrain.source_lod(key);
    for (size_t i = 0; i < stamp.sources.size(); ++i)
    {
      const auto neighbor = TerrainMeshInput::Neighbor(key, i);
      if (TerrainMeshInput::ValidNeighbor(neighbor))
        stamp.sources[i] =
            i ? terrain.source_face_revision(neighbor, (i - 1) ^ 1)
              : terrain.source_revision(neighbor);
    }
    return stamp;
  }
};
struct TerrainMeshResult
{
  RegionKey key;
  TerrainMeshStamp stamp;
  Lighthouse::Mesh3D mesh;
  std::string error;
};

enum class TerrainWorkerPhase
{
  Idle,
  Starting,
  Meshing,
  Ready
};
struct TerrainWorkerStatus
{
  size_t slot = 0;
  TerrainWorkerPhase phase = TerrainWorkerPhase::Idle;
  std::string threadId;
  std::optional<RegionKey> chunk;
  uint32_t lod = 0;
  double milliseconds = 0;
  std::string lastResult;
  bool retiring = false;
};

// Configurable bounded CPU workers and two completed results. Snapshots only
// retain immutable sources; all building/disposal happens on the workers.
class TerrainMeshScheduler
{
public:
  static constexpr size_t default_meshing_threads = 3;
  static constexpr size_t maximum_meshing_threads = 64;
  static constexpr size_t worker_limit = default_meshing_threads;
  using Builder = std::function<Lighthouse::Mesh3D(const TerrainMeshInput&)>;
  explicit TerrainMeshScheduler(Builder builder =
                                    [](const TerrainMeshInput& input)
                                { return TerrainMesher::Build(input); })
      : builder_(std::move(builder))
  {
    if (!builder_)
      throw std::invalid_argument("Meshing requires a CPU mesh builder");
    EnsureActivities(threadCount_);
  }
  size_t MeshingThreadCount() const noexcept
  {
    return threadCount_;
  }
  void SetMeshingThreadCount(size_t count)
  {
    if (!count || count > maximum_meshing_threads)
      throw std::invalid_argument(
          "Meshing thread count must be between 1 and 64");
    if (count == threadCount_)
      return;
    EnsureActivities(count);
    threadCount_ = count;
    // Existing CPU jobs finish in the background. Never destroy/join a live
    // future from a render-thread control, even when reducing the count.
    TrimIdleActivities();
  }
  size_t RetiringWorkers() const
  {
    return std::count_if(workers_.begin(), workers_.end(),
                         [&](const auto& worker)
                         { return worker.slot >= threadCount_; });
  }
  std::vector<TerrainWorkerStatus> Workers() const
  {
    std::vector<TerrainWorkerStatus> result;
    for (const auto& activity : activity_)
    {
      std::unique_lock lock(activity->mutex, std::try_to_lock);
      if (!lock)
      {
        TerrainWorkerStatus status;
        status.slot = result.size() + 1;
        status.phase = TerrainWorkerPhase::Starting;
        status.retiring = status.slot > threadCount_;
        status.lastResult = "Worker updating status";
        result.push_back(std::move(status));
        continue;
      }
      auto status = activity->status;
      status.retiring = status.slot > threadCount_;
      if (status.phase == TerrainWorkerPhase::Starting ||
          status.phase == TerrainWorkerPhase::Meshing)
        status.milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - activity->started)
                .count();
      result.push_back(std::move(status));
    }
    return result;
  }
  void Update(const WorldTerrain& terrain)
  {
    if (revision_ != terrain.revision())
    {
      desired_.clear();
      for (auto key : terrain.source_keys())
        desired_.emplace(key, TerrainMeshStamp::Read(terrain, key));
      std::erase_if(submitted_, [&](const auto& entry)
                    { return !desired_.contains(entry.first); });
      revision_ = terrain.revision();
    }
    for (auto it = workers_.begin(); it != workers_.end();)
    {
      if (!it->cleanup && ready_.size() >= 2)
      {
        ++it;
        continue;
      }
      if (it->future.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
      {
        ++it;
        continue;
      }
      const bool cleanup = it->cleanup;
      auto result = it->future.get();
      {
        std::lock_guard lock(activity_[it->slot]->mutex);
        auto& status = activity_[it->slot]->status;
        status.phase = TerrainWorkerPhase::Idle;
        status.lastResult = cleanup ? "Released obsolete CPU meshes"
                            : !result.error.empty() ? result.error
                            : Current(result)
                                ? "Completed; result queued for upload"
                                : "Stale result discarded";
      }
      it = workers_.erase(it);
      if (cleanup)
        continue;
      if (Current(result))
        ready_.push_back(std::move(result));
      else
      {
        ++discarded_;
        if (!result.mesh.vertices.empty() || !result.mesh.indices.empty())
          garbage_.push_back(std::move(result.mesh));
      }
    }
    TrimIdleActivities();
    if (workers_.size() >= threadCount_ || ready_.size() >= 2)
      return;
    // Stable ordering avoids starvation/repeated snapshots during motion.
    for (const auto& [key, stamp] : desired_)
    {
      if (std::any_of(workers_.begin(), workers_.end(), [&](const auto& worker)
                      { return worker.key && *worker.key == key; }))
        continue;
      const auto previous = submitted_.find(key);
      if (previous != submitted_.end() && previous->second == stamp)
        continue;
      const auto available = FreeSlot();
      if (!available)
        break;
      auto snapshot = TerrainMeshInput::Capture(terrain, key, stamp.lod);
      const size_t slot = *available;
      const auto activity = activity_[slot];
      {
        std::lock_guard lock(activity->mutex);
        activity->status.phase = TerrainWorkerPhase::Starting;
        activity->status.threadId.clear();
        activity->status.chunk = key;
        activity->status.lod = stamp.lod;
        activity->status.lastResult.clear();
        activity->status.milliseconds = 0;
        activity->started = std::chrono::steady_clock::now();
      }
      auto garbage = std::move(garbage_);
      auto future = std::async(
          std::launch::async,
          [key, stamp, activity, build = builder_, garbage = std::move(garbage),
           input = std::move(snapshot)]() mutable
          {
            {
              std::lock_guard lock(activity->mutex);
              NameTerrainMeshThread(activity->status.slot);
              activity->status.threadId = TerrainThreadId();
              activity->status.phase = TerrainWorkerPhase::Meshing;
            }
            garbage.clear();
            garbage.shrink_to_fit();
            TerrainMeshResult result{key, stamp, {}, {}};
            try
            {
              result.mesh = build(input);
            }
            catch (const std::exception& error)
            {
              result.error = error.what();
            }
            input = {}; // release source snapshot references on the worker
            {
              std::lock_guard lock(activity->mutex);
              activity->status.phase = TerrainWorkerPhase::Ready;
              activity->status.milliseconds =
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - activity->started)
                      .count();
            }
            return result;
          });
      workers_.push_back({std::move(future), slot, false, key});
      submitted_.insert_or_assign(key, stamp);
      ++scheduled_;
      if (workers_.size() >= threadCount_)
        break;
    }
    if (!garbage_.empty() && workers_.size() < threadCount_)
    {
      const auto available = FreeSlot();
      if (!available)
        return;
      const size_t slot = *available;
      const auto activity = activity_[slot];
      {
        std::lock_guard lock(activity->mutex);
        activity->status.chunk.reset();
        activity->status.threadId.clear();
        activity->status.phase = TerrainWorkerPhase::Starting;
        activity->status.lastResult = "Releasing obsolete CPU meshes";
        activity->started = std::chrono::steady_clock::now();
      }
      auto garbage = std::move(garbage_);
      auto future = std::async(
          std::launch::async,
          [activity, garbage = std::move(garbage)]() mutable
          {
            {
              std::lock_guard lock(activity->mutex);
              NameTerrainMeshThread(activity->status.slot);
              activity->status.threadId = TerrainThreadId();
              activity->status.phase = TerrainWorkerPhase::Meshing;
            }
            garbage.clear();
            garbage.shrink_to_fit();
            {
              std::lock_guard lock(activity->mutex);
              activity->status.phase = TerrainWorkerPhase::Ready;
              activity->status.milliseconds =
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - activity->started)
                      .count();
            }
            return TerrainMeshResult{};
          });
      workers_.push_back({std::move(future), slot, true, {}});
    }
  }
  bool Contains(RegionKey key) const
  {
    return desired_.contains(key);
  }
  bool Current(const TerrainMeshResult& result) const
  {
    const auto it = desired_.find(result.key);
    return it != desired_.end() && it->second == result.stamp;
  }
  std::optional<TerrainMeshResult> TakeReady()
  {
    if (ready_.empty())
      return {};
    auto result = std::move(ready_.front());
    ready_.pop_front();
    return result;
  }
  size_t Pending() const
  {
    return workers_.size() + ready_.size() + garbage_.size();
  }
  uint64_t Scheduled() const
  {
    return scheduled_;
  }
  uint64_t Discarded() const
  {
    return discarded_;
  }

private:
  // Per-source revisions make unrelated chunks immune to edits/LOD changes.
  std::unordered_map<RegionKey, TerrainMeshStamp, RegionKeyHash> desired_,
      submitted_;
  uint64_t revision_ = UINT64_MAX, scheduled_ = 0, discarded_ = 0;
  struct WorkerActivity
  {
    std::mutex mutex;
    TerrainWorkerStatus status;
    std::chrono::steady_clock::time_point started;
  };
  struct WorkerJob
  {
    std::future<TerrainMeshResult> future;
    size_t slot;
    bool cleanup = false;
    std::optional<RegionKey> key;
  };
  std::optional<size_t> FreeSlot() const
  {
    if (workers_.size() >= threadCount_)
      return {};
    for (size_t i = 0; i < threadCount_; ++i)
      if (std::none_of(workers_.begin(), workers_.end(),
                       [i](const auto& worker) { return worker.slot == i; }))
        return i;
    return {};
  }
  void EnsureActivities(size_t count)
  {
    while (activity_.size() < count)
    {
      auto activity = std::make_shared<WorkerActivity>();
      activity->status.slot = activity_.size() + 1;
      activity_.push_back(std::move(activity));
    }
  }
  void TrimIdleActivities()
  {
    while (activity_.size() > threadCount_)
    {
      const auto slot = activity_.size() - 1;
      if (std::any_of(workers_.begin(), workers_.end(),
                      [slot](const auto& worker)
                      { return worker.slot == slot; }))
        break;
      activity_.pop_back();
    }
  }
  size_t threadCount_ = default_meshing_threads;
  Builder builder_;
  std::vector<std::shared_ptr<WorkerActivity>> activity_;
  std::vector<WorkerJob> workers_;
  std::deque<TerrainMeshResult> ready_;
  std::vector<Lighthouse::Mesh3D> garbage_;
};
} // namespace EESG
