#pragma once
#include <Lighthouse/Engine/Rendering/IRenderDevice.hpp>
#include <cstring>

namespace EESG
{
// These application-owned buffers are reused by multiple material passes.
inline void WriteHostBuffer(Lighthouse::IRenderDevice& device,
                            Lighthouse::BufferHandle buffer,
                            std::span<const std::byte> bytes)
{
  const auto fence = device.CreateFence();
  try
  {
    device.GetQueue(0)->Submit(Lighthouse::QueueSubmitInfo{}, fence);
    device.WaitForFence(fence);
    auto mapped = device.MapMemory(buffer);
    std::memcpy(mapped.data(), bytes.data(), bytes.size());
    device.FlushMappedMemoryRange(buffer, 0, bytes.size());
    device.UnmapMemory(buffer);
  }
  catch (...)
  {
    device.DestroyFence(fence);
    throw;
  }
  device.DestroyFence(fence);
}
} // namespace EESG
