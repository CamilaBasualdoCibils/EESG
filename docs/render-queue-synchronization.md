# Render queues and synchronization

`Lighthouse::IRenderDevice` has no submission or presentation methods. Both
operations belong to `IRenderQueue`; resource creation/destruction, memory
mapping and host synchronization belong to the device. Queue submission is
asynchronous. The caller retains command lists, resources and synchronization
objects until completion and externally synchronizes access to a queue.

```cpp
auto surface = backend.CreateSurface(window);
auto device = backend.CreateDevice(RenderDeviceCreateInfo{
    .queues = {{
        .capabilities = RenderQueueCapability::Graphics |
                        RenderQueueCapability::Present,
        .minCount = 1,
        .preferredCount = 2,
        .presentSurface = surface,
    }},
});
auto* queue = device->GetQueue(0);
auto swapchain = device->CreateSwapchain(surface, config);
// Acquire, record with device->CreateCommandList(queueIndex), then submit.
queue->Present(swapchain);
device->DestroySwapchain(swapchain);
device.reset();
backend.DestroySurface(surface);
// Destroy the native window last.
```

`GetQueue(uint32_t index)` is a lookup only. It returns a stable pointer to a
queue created with the device, or null for an unavailable index. It never
matches requests, allocates queues, or aliases a missing slot to another queue.
Command-list creation takes the same queue index so Vulkan command pools use
the corresponding native family. Queues are indexed in request order, skipping
unfulfilled optional counts.

`RenderQueueRequest` contains a bitmask enum `capabilities`, `minCount = 0`,
`preferredCount = 1`, and `presentSurface = {}`. Minimums are satisfied across
all requests before preferences consume remaining capacity. A minimum greater
than the preferred count is invalid. An unmet minimum fails device creation;
a shortfall in preferred counts is allowed. Vulkan matches real native queue
slots and emits grouped `VkDeviceQueueCreateInfo` entries. Dedicated families
are preferred for requests needing fewer capabilities. No hidden queue slots
are added. This backend requires at least one command-capable queue.

OpenGL creates independent shared EGL contexts with one worker thread per queue.
Minimums are allocated before preferences; context/thread availability and a
64-worker device budget bound the available count. See [the OpenGL implementation report](opengl-render-queues.md) for ownership, synchronization and limitations.

For an upfront presentation guarantee, create the window using the backend's
`RequiresEGLWindow()` requirement, then register the window with the backend
before device creation, and use that handle in the creation request. The window
must outlive every device using it. Because this architecture owns its Vulkan
instance inside the render device, backend surface registration identifies the
native window target; its `VkSurfaceKHR` is materialized before physical-device
and queue-family selection. A requested family is checked against that native
surface before logical-device creation. OpenGL validates the target's EGL
presentation formats. Native device surfaces are released with the device;
backend `DestroySurface` releases the registered target.

Existing late-window creation through `IRenderDevice::CreateSurface` remains
available. It cannot create additional queues. Its swapchain creation and
presentation must be supported by an already-created queue. The runtime now
explicitly requests its general graphics/compute/transfer queue in the device
description, and its window views use queue zero.

Vulkan resources and swapchains use concurrent sharing across distinct created
queue families, since the existing resource interface does not expose exclusive
ownership-transfer barriers. Callers still supply semaphore dependencies for
work crossing queues. `Queue::Present` accepts binary semaphore waits for
rendering submitted to another queue; its internal presentation transition is
ordered on the device's graphics queue before presentation on the selected
queue. No asynchronous-compute performance guarantee is advertised.

## Removed functions

| Former device operation | Replacement |
| --- | --- |
| `UpdateBuffer` | Application-owned host-accessible buffers, `MapMemory`, copy bytes, `FlushMappedMemoryRange`, `UnmapMemory`; wait for prior GPU consumers before overwriting storage. |
| `MapStreamingBuffer` | `MapMemory`; host access comes from `BufferDesc::memory.requiredAccess`. |
| `ReadCompletedBuffer` | Command-list `CopyBuffer`, signal/wait a timeline semaphore, invalidate the destination allocation, map/read/unmap. |
| `StreamingBufferAlignment` | Removed; no consumers existed. |
| `PublishStreamingWrites` | `FlushMappedMemoryRange`; OpenGL uses coherent mappings. |
| `InsertCompletionMarker` | Create a fence and pass it to a queue submission. Empty submissions are supported for an existing queue tail. |
| `PollCompletionMarker` | `GetFenceStatus`; terrain instead polls `WaitSemaphore(value, 0)`. |
| `ReleaseCompletionMarker` | `DestroyFence` or destruction of the owned timeline semaphore. |

No streaming buffer type or mapping-permission checks were introduced. Vulkan
uses VMA mapping/flush/invalidate operations and requests host-visible allocation
only for CPU access. Existing buffer initial-data support is preserved: device
allocations are initialized via a temporary host transfer buffer. OpenGL enables
mapping storage flags only for the requested CPU access, maps on `MapMemory`,
and unmaps on `UnmapMemory`.

## Wait audit

The audit covers EESG source/tests and Lighthouse source, headers, tests and
examples, including `WaitIdle`, `waitIdle`, `glFinish`, native fence waits and
completion-marker operations.

| Location | Result and reason |
| --- | --- |
| Backend buffer updates | Removed implicit full-device waits with the old API. |
| Vulkan resource destruction and descriptor updates | Removed implicit full-device waits; the owner is responsible for lifetime synchronization. |
| `RenderGraph::Execute`, `Drain`, `Clear` | A fence tracks graph completion and retains command lists, allocations and retirement callbacks. Drain waits on that fence. The next execution drains the previous one because imported host buffers and descriptor sets currently have one set of storage. The exceptional cleanup path retains a device wait before freeing potentially submitted work. |
| `EntityRenderer::Draw`, `WorldRenderer` light updates | Normal host-accessible buffers. An application-local queue-tail fence protects overwriting storage used by earlier material passes. This preserves the existing blocking upload behavior without a hidden device wait. |
| `TerrainSystem::PollReadbacks` | Readback copy submissions signal per-slot timeline values. A zero-time host semaphore wait precedes invalidation and reading. No device idle in polling. |
| OpenGL context ownership | Independent queue workers own their contexts and local debug/state caches. Resource initialization and cross-queue dependencies use flushed producer `GLsync` and GPU `glWaitSync`; no global execution lock. |
| Vulkan `Queue::Present` / acquisition | Removed the per-frame queue-idle wait and the blocking immediate transition submission. A retained transition command/fence is reused after waiting at the next acquisition. Render-finished semaphores are per swapchain image. |
| `RenderMeshManager::Upload` | Replaced device idle with an explicit queue-tail fence before replacing the previous shared mesh buffers. |
| Backend `ReadImage` | Existing blocking diagnostic API remains. Vulkan uses its immediate submission fence and a CPU-readable transfer buffer. OpenGL marshals fence issuance to each queue, waits server-side on the resource context, then performs blocking texture readback without a device idle. |
| Vulkan swapchain resize/out-of-date/destruction | Retained idle waits before replacing/destroying presentation resources. Present-wait/present fences are not enabled by this backend; a rendering completion fence alone does not prove that the presentation engine released a swapchain. |
| `GameManager` shutdown; `WorldRenderer`, `EntityRenderer`, `TerrainSystem` release; material destruction/reload; `RenderMeshManager::Release` | Retained explicit owner-level device waits when bulk resources are torn down or rebuilt. These are lifecycle boundaries, not per-draw backend waits. |
| Vulkan ImGui shutdown and device destruction | Retained lifecycle device waits. |
| Explicit device/queue `WaitIdle` APIs | Retained as native operations. OpenGL uses GPU completion fences on each worker; no `glFinish`. |

## Backend semantics and limits

Vulkan fences and binary/timeline semaphores map to native objects. Timeline
support is enabled and required at device selection. Wait/signal values and
synchronization2 stage masks are passed to `submit2`. Queue state is committed
only after submission succeeds. GPU debug groups/markers use Vulkan debug utils
when the extension entrypoints are available.

OpenGL uses `GLsync` for completion and binary semaphore emulation. Timeline
values retain ordered sync objects until queried/completed; host signal/query/
wait operations expose the completed counter. Stage masks conservatively cover
all work. Debug commands use `glPushDebugGroup`, `glPopDebugGroup` and
`glDebugMessageInsert`; OpenGL has no matching label-color parameter.

OpenGL queue waits require an already submitted matching producer signal. A
worker may wait for CPU issuance of the producer's sync object, then uses a GPU
wait for completion. Unsubmitted future waits fail explicitly, except zero-time
host polling returns false. Host signaling while GPU timeline signals remain
pending is unsupported. EXT/NV native semaphores require external backing, for
which this project has no provider; advertising the extension is insufficient.
See [the OpenGL report](opengl-render-queues.md) for the full capability contract.

The graph currently has one in-flight execution. Per-pass host updates can also
wait for earlier consumers. Removing these waits requires application-owned
per-frame buffers and descriptor sets; generic streaming/ring-buffer policy was
not put into the render device.

The nested VISR dependency has a separate `VISR::Render::IRenderDevice` and its
own renderer/backends. It does not implement Lighthouse's device contract and
was not migrated. No additional Lighthouse graphics backends were found.

Synchronization and queue creation were checked against the [Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html) and [device queue creation requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceCreateInfo.html).

## Validation

The Debug EESG application and both graphics backends build with the existing
cached dependencies. The suite includes new cross-backend tests for signaled/
reset/reused fences, empty and batched submissions, binary semaphore reuse,
timeline host/GPU signaling, polling, offset buffer copies and mapped readback.
Window tests cover pre-device surface-constrained queue requests, destroying a surface
before its swapchain, stale surface handles, debug groups spanning context
switches, multiple swapchains, resize and presentation. Creation tests cover minimum/preferred counts, impossible requests, stable indexed lookups, and cross-queue copies when more than one native queue is available.

- Debug EESG application, OpenGL backend, Vulkan backend and EESG test executables: build passed.
- The earlier queue-creation phase passed **68 tests with 1 native-queue availability skip (69 total)**. Current multicontext OpenGL validation is recorded in [the OpenGL report](opengl-render-queues.md).
- ImGui-disabled build: OpenGL backend, Vulkan backend, renderer-technique and render-graph inspection targets built successfully; **6/6 focused tests passed**.
- No Vulkan validation-layer instrumentation was used in these runs.

Dependencies were reused from the existing local vcpkg/FrameGraph cache. No dependency upgrades were made. The existing staged engine edits were preserved, and these changes were left unstaged.
