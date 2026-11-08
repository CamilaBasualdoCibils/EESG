#pragma once
#include <sstream>
#include <string>
#include <thread>
#if defined(__linux__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace EESG
{
inline std::string TerrainThreadId()
{
#if defined(__linux__)
  // Kernel TIDs match the thread identifiers displayed by perf/Hotspot.
  return std::to_string(::syscall(SYS_gettid));
#else
  std::ostringstream id;
  id << std::this_thread::get_id();
  return id.str();
#endif
}
inline void NameTerrainMeshThread(size_t slot)
{
#if defined(__linux__)
  const auto name = "TerrainMesh" + std::to_string(slot);
  ::pthread_setname_np(::pthread_self(), name.c_str());
#else
  (void)slot;
#endif
}
} // namespace EESG

#include "World/Terrain/WorldTerrain.hpp"
#include <chrono>
#include <mutex>
#include <optional>
namespace EESG
{
struct TerrainBackgroundStatus
{
  std::string threadId, state = "Idle", lastResult;
  std::optional<RegionKey> chunk;
  uint32_t lod = 0;
  double milliseconds = 0;
  bool active = false;
};
class TerrainBackgroundActivity
{
public:
  void Begin(std::optional<RegionKey> key, uint32_t lod)
  {
    std::lock_guard lock(mutex_);
    status_ = {};
    status_.chunk = key;
    status_.lod = lod;
    status_.active = true;
    status_.state = "Starting";
    started_ = std::chrono::steady_clock::now();
  }
  void Mark(const char* state, const char* name)
  {
#if defined(__linux__)
    ::pthread_setname_np(::pthread_self(), name);
#else
    (void)name;
#endif
    std::lock_guard lock(mutex_);
    status_.state = state;
    status_.threadId = TerrainThreadId();
  }
  void Finish(std::string outcome)
  {
    std::lock_guard lock(mutex_);
    status_.active = false;
    status_.state = "Idle";
    status_.lastResult = std::move(outcome);
    status_.milliseconds = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - started_)
                               .count();
  }
  TerrainBackgroundStatus Snapshot() const
  {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock)
    {
      TerrainBackgroundStatus busy;
      busy.state = "Updating status";
      return busy;
    }
    auto copy = status_;
    if (copy.active)
      copy.milliseconds = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started_)
                              .count();
    return copy;
  }

private:
  mutable std::mutex mutex_;
  TerrainBackgroundStatus status_;
  std::chrono::steady_clock::time_point started_;
};
} // namespace EESG
