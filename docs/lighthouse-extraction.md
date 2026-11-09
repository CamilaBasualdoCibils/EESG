# Lighthouse extraction migration

## Local state

The game consumes `extern/Lighthouse-Extended`; Extended consumes `extern/Lighthouse-Engine`. Libraries are `Lighthouse::Engine` and `Lighthouse::Extended`, with separate `Lighthouse::EngineEmbedded` and `Lighthouse::ExtendedEmbedded` resource targets. No commits or pushes were made.

The original direct Engine checkout was already at `b9fddd28c5584bac0142ec705b0e09a5d44e7b10`, ahead of EESG's recorded gitlink. That revision is preserved in the nested checkout. The original pristine working tree was relocated, with approval, to the sibling directory `EESG-engine-recovery-20261108`. Its original Git metadata and branches remain in EESG's `.git/modules/extern/Lighthouse-Engine`; all original HEADs/refs and clean working trees were checked after relocation. A full pre-extraction source/history archive is also at `/tmp/eesg-before-extraction-20261108.tar.gz`.

The old direct Engine gitlink deletion and `.gitmodules` update are staged. Extended's nested Engine registration is staged. Other migration changes are uncommitted. Extended has no HEAD because its remote was empty. Consequently, the outer Extended gitlink cannot yet be added to EESG's index, and fresh remote recursive clones cannot reproduce these changes until authorized commits and publication occur.

## Ownership and API changes

- Engine's `Renderer` becomes `RenderGraphComposer`. Its device, queues, graph, views, techniques, synchronization, application accessors, and lifecycle retain their responsibilities.
- Extended owns `Scene`, generic components, `CameraView`, materials, scene rendering and inspection. Engine owns Assets, Mesh, ImageData and glTF loading. Extended headers use `Lighthouse/Extended/`; Engine asset headers use `Lighthouse/Engine/Assets/`. Types use namespace `Lighthouse`.
- EESG `World` contains a generic Scene plus terrain systems and observers. `TerrainCamera` keeps terrain observer ownership in EESG. Player/gameplay stay in EESG.
- Extended `Renderer` runs six ordered `IRenderExtension` stages per camera. EESG's `TerrainRenderer` registers world residency once per graph build using the build ID. Existing terrain code has residency passes but no meshing/culling/G-buffer implementation; no such features were invented.
- Generic built-in shaders are embedded in Extended. Helmet/model/game shaders remain game-owned. Game asset paths are derived from configured source roots, so launching from another directory works.

## Test migration

- Backend, queue, viewport, graph, CPU/raw mesh, glTF loading, image data/upload, buffer/upload, input and window tests belong to Engine.
- Materials, generic scene/entity, component inspection and textured scene-rendering tests belong to Extended. The mesh assertions now live together in Engine. Stale setters/destruction calls were adapted to the current constructor/device API.
- Terrain residency, adaptive octree and the game glTF asset check belong to EESG.
- Applicable SceneEntityTests and SceneInspectorTests assertions were retained and adapted. Removed legacy active-camera assertions refer to an API that no longer exists; current multi-camera regression tests cover the replacement camera-view path.
- Legacy Engine RendererTests referred to absent SceneHandle/CameraSource/RenderViewDesc APIs; WorldRendererTests referred to absent RenderScene/IWorldRenderSystem/MeshRenderer APIs and hierarchy rendering behavior absent from the current implementation. They were removed under the instruction to remove tests whose code is gone. Current composer and GPU scene integration tests are retained.
- Legacy Engine MaterialTests referred to unimplemented Assets::LoadMaterial and missing Slang material fixtures. It was removed; the actual GLSL material implementation and its tests moved from EESG to Extended. The obsolete two-shader Engine Material declaration was removed to allow the existing Extended material type in namespace Lighthouse.

## Verification

- Engine standalone: library and window example build; 117/117 CTest tests passed.
- Extended standalone: library and tests build; 128/128 CTest tests passed, including its Engine dependency tests.
- EESG combined: build and 131/131 CTest tests passed, including the three game-specific tests.
- The complete combined suite passed with Extended/res temporarily renamed, proving built-in material resources are not required at runtime.
- A separate generator check covered binary bytes, NULs, an empty file and a missing resource, ran with its source resource directory absent, and verified regeneration after file edits, additions and removals.
- EESG launched from `/tmp` and remained running for five seconds with each of OpenGL and Vulkan before deliberate termination. Pixel regressions verify rendering; this smoke test is not a manual visual inspection.

- Clean source exports without Git metadata built and ran two unrelated applications: Engine alone, and Extended with Engine already added. Both used ImGui and the optional Lighthouse backends disabled; the second verified duplicate Engine initialization is avoided. These were clean-source export checks, not fresh remote Git clones, which remain blocked by the explicit no-commit instruction and Extended's empty history.
- The usual EESG `build/` directory was reconfigured and rebuilt, and its three game-owned CTest tests passed there as well.

Validation used existing installed dependencies and the cached FrameGraph checkout to avoid reinstalling dependencies. Those machine-specific paths were command-line validation inputs, not repository build requirements. Window tests required desktop display access; the restricted sandbox could not connect to display :0.

## Asset ownership correction

Assets, ImageData, Mesh and the glTF loader now live in Lighthouse-Engine, including their CPU/GPU upload tests. Extended and EESG use the canonical `Lighthouse/Engine/Assets/` headers. The loader representation and behavior are unchanged. The scene importer task remains deferred. No commits or staged content were changed by this correction.

Validation after the asset ownership correction: Engine standalone 123/123 tests passed; Extended standalone 129/129 passed (including Engine tests); the three game-owned tests passed from the rebuilt EESG build directory.
