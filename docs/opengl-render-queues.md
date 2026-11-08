# Multicontext OpenGL render queues

## Implementation and ownership

Queues are created during device initialization from `RenderQueueRequest`. All minimums are allocated before preferred counts. `GetQueue(index)` only looks up an existing queue. Each queue has a distinct shared EGL context, pbuffer, worker thread and condition-variable FIFO. Submission copies recorded commands and resource/descriptor metadata, then returns without waiting for execution or GPU completion. Command recording requires no current GL context. Graphics queues also support compute and transfer.

| Owner | Context and state |
| --- | --- |
| Backend device group | Existing root share-group context and EGL display. |
| Device resource APIs | One shared resource context, scoped onto the calling thread under a resource-only mutex. The caller's EGL API/context is restored. This mutex never surrounds queue execution. |
| Each queue | One context owned by one worker thread; local VAOs, FBOs, bindings and debug-group state. FIFO work uses the existing command replay implementation. |
| Each host synchronization thread | Lazily cached shared context/pbuffer for polling and explicit host waits. GPU waits do not hold the resource-context mutex. These contexts are detached after use and released with the device. |
| Each swapchain | Native EGL surface and a surface mutex. It does not own a queue context. Presentation temporarily binds the presenting worker's context to the surface and restores its pbuffer before returning. |

Queue workers execute independently; there is no device-wide GL execution mutex or submission polling loop. A test deliberately blocks one worker's CPU job and verifies that the other worker progresses. Actual GPU scheduling overlap remains driver-dependent, so the existing asynchronous-compute performance capability remains conservative.

Textures, buffers, shaders, monolithic programs and samplers remain shared device objects. Handles identify the same native allocation on every queue. Submission snapshots retain native resource lifetimes and descriptor contents through GPU completion, even if the caller clears/destroys its command list or releases a logical resource handle after submission. Snapshots copy metadata, not GPU allocations or shader bytecode. This retention is an OpenGL implementation detail; callers should continue observing the cross-backend lifetime contract.

VAOs are cached per queue and invalidated when a pipeline is retired. Render-pass and presentation FBOs are created/deleted on their executing context. The backend does not currently create program-pipeline, query or transform-feedback objects; none were moved into shared storage. Shared-object deletion is deferred onto a valid shared context after the final retained reference is released.

## Synchronization

Completion fences use flushed `GLsync` objects and become complete only after GPU work completes. Queue dependencies use `glWaitSync`, with conservative memory barriers before consumption. Resource API writes publish a flushed synchronization point for subsequent queue snapshots. No `glFinish` remains in the OpenGL backend; explicit queue/device idle and shutdown use completion fences.

Binary semaphore signals can be consumed once. Timeline signals must increase monotonically. Cross-queue binary dependencies and ordered timeline dependencies work when the producing signal has already been submitted. A consumer worker may wait on a condition variable for that producer to **issue and publish** its sync object; the GPU dependency itself is a server-side wait, not a CPU wait for GPU completion. Producer errors are published so dependent workers can fail rather than hang.

An unsubmitted future GPU timeline wait is explicitly unsupported and throws through the existing exception mechanism. A zero-time host query for such a value returns false; a blocking host wait is unsupported. Host signaling while GPU timeline signals remain pending is also explicitly unsupported. These cases are covered by tests, including rejection followed by a later valid signal/submission.

`GL_EXT_semaphore`, `GL_EXT_semaphore_fd` and `GL_NV_timeline_semaphore` were investigated. EXT semaphore names require external backing; NV creation likewise does not associate external backing. Lighthouse has no external semaphore allocation/import provider, so extension availability alone does not enable this path. No unbacked native semaphore is used and no hidden Vulkan device was added. Full future-value semantics would require a deliberately designed backing/import provider. See the [EXT external-object specification](https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/extensions/EXT/EXT_external_objects.txt), [FD import specification](https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/extensions/EXT/EXT_external_objects_fd.txt) and [NV timeline specification](https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/extensions/NV/NV_timeline_semaphore.txt).

On the tested Radeon RX 5700 XT/Mesa driver, all three extension strings are advertised; the backing provider remains unavailable. Software llvmpipe testing reported none of the three. Both use the same supported GLsync fallback.

## Presentation and shutdown

A queue requested for a particular surface rejects a different surface. Queues use `EGL_KHR_no_config_context` when available to support compatible surfaces on the same display; otherwise EGL validates fixed-configuration compatibility and incompatible binding fails explicitly. This follows the [EGL configless-context specification](https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/extensions/KHR/EGL_KHR_no_config_context.txt). Binary presentation waits are GPU-side. Presentation marshals to the owning worker and returns after issuing the swap and restoring the pbuffer; swap/vsync behavior can naturally block the presenting worker.

Resizing/destroying one swapchain does not replace unrelated queue contexts. Pending offscreen work retains its images; a destroyed target is marked dead and cannot subsequently be presented. Window creation/destruction retains GLFW's existing thread and lifetime requirements.

Shutdown first stops acceptance, drains queued jobs and GPU completion, deletes local objects on each worker, releases worker contexts and joins threads. Shared resources and native surfaces are then released on the resource context. Device destruction must not race new external API calls, as with the existing device lifetime contract.

## Validation and remaining concerns

Focused tests cover independent native worker/context identities and CPU progress; two concurrent submitters; immediate command-list destruction; shared-buffer lifetime; cross-queue binary and ordered timeline copies; unsupported future waits; compute on graphics queues; local VAO/FBO rendering and descriptor snapshots; fence completion; queued-work shutdown; resource API calls from two CPU threads; surface affinity and independent resize/presentation. Existing shader, mesh-upload, window and ImGui regressions are also exercised.

The implementation bounds device queue workers at 64; an unsatisfied minimum fails creation and preferred counts may be truncated by context/thread availability. Host wait contexts are cached by calling thread until device destruction. Full resource-table metadata snapshots favor safe existing handles over a new resource-tracking abstraction; their CPU cost and coarse resource retention are a potential optimization target. Completed snapshots are collected at subsequent queue jobs, host completion queries or queue idle/shutdown, so an unused idle queue may retain its last completed snapshot until one of those events.

The render graph's existing single-in-flight policy is unchanged. Shared-resource data hazards still require application-provided synchronization; snapshots do not serialize conflicting GPU writes. Public device/queue/command-list/graph/handle APIs and Vulkan/DirectX backends were not changed during this OpenGL task.

Final Debug validation:

- EESG application, both graphics backends and EESG test executables build successfully.
- Full EESG suite: **76/78 passed**, including all **9 new OpenGL multiqueue tests**. The two ImGui viewport pixel tests (OpenGL and Vulkan) timed out in the current display environment.
- Existing OpenGL shader, swapchain, window and mesh regression selection: **7/7 passed**. The broader ImGui regression run was stopped after two viewport tests timed out.
- ImGui-disabled OpenGL/Vulkan and focused renderer/graph targets build; **6/6 focused tests passed**. The unrelated unbuilt legacy aggregate test was excluded from this focused run.
- The debugger located the ImGui stall inside GLFW `glfwShowWindow` / X11 `XCheckTypedWindowEvent`, with the queue worker idle on its condition variable. The unchanged original executable reproduces the same viewport stall. This display-dependent coverage remains unresolved; the full suite is not claimed clean.
- No Vulkan validation-layer instrumentation or dependency upgrades were used. Vulkan and public interface files remain byte-identical to the start of this OpenGL task.
