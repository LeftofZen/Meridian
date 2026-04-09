# Meridian
A next-gen game engine

# Architecture Guide: Path Traced Procedural Engine
This document outlines the core data structures and acceleration strategies for implementing a high-performance path tracer within a procedural, infinite, and simulation-heavy environment.
## 1. High-Level Architecture: Two-Level Acceleration
To handle dynamic entities (NPCs, players) alongside a static world, we utilize a **Two-Level Acceleration Structure (TLAS/BLAS)**.
 * **Bottom-Level (BLAS):** Contains local geometry (Triangles for NPCs, SVOs for terrain).
 * **Top-Level (TLAS):** Contains instances of BLAS objects with transform matrices.
   * *Implementation Note:* Update only the TLAS matrices during the simulation loop to avoid expensive geometry rebuilds.
## 2. Terrain Data: Sparse Voxel Octree (SVO)
Traditional meshes are unsuitable for infinite, editable terrain. We use an **SVO** to represent the world. The world heightmap is procedurally generated in chunks using the methods outlined in this blog post: https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html?m=1.
 * **Efficiency:** O(\log n) traversal using a **Digital Differential Analyzer (DDA)** algorithm.
 * **Editability:** Real-time "digging" or "building" only requires updating local leaf nodes and propagating values upward.
 * **LOD:** The tree depth inherently provides Levels of Detail; distant rays can stop at higher-level nodes to save cycles.
## 3. Global Management: Spatial Hash Grid
For an infinite world, we manage memory through a **Spatial Hash Grid** that stores active chunks.
 * **Key:** Chunk Coordinates (e.g., x, y, z hashed to a uint64).
 * **Value:** Pointer to a local SVO/BLAS.
 * **Workflow:**
   1. Ray exits current chunk bounds.
   2. Query Hash Grid for the neighbor chunk.
   3. If chunk exists, continue ray-march; if not, return "sky" or trigger procedural generation.
## 4. Entity Handling: Dynamic BVH
For characters and physics-driven objects that deform or move rapidly:
 * **Structure:** A specialized **Bounding Volume Hierarchy** that supports "refitting" (adjusting bounds without a full rebuild).
 * **Integration:** These BVHs are referenced as instances within the **TLAS**, allowing rays to transition seamlessly between voxel terrain and polygonal NPCs.
## 5. Performance Checklist
| Strategy | Purpose |
|---|---|
| **DDA Stepping** | Fast ray-traversal through voxels. |
| **Temporal Re-projection** | Smear lighting data across frames to reduce noise (e.g., ASVGF). |
| **Stochastic Updates** | Prioritize updating acceleration structures for objects closest to the camera. |
| **Ray Binning** | Group rays by direction to improve GPU cache hits. |
## 6. Technology Stack Decisions

The following library selections were evaluated against the requirements of a real-time, sim-heavy, path-traced voxel game engine.

### Physics — Jolt Physics

**Selected:** [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
**vcpkg package:** `joltphysics`

| Candidate | Verdict | Notes |
|---|---|---|
| **Jolt Physics** | ✅ **Selected** | Multi-threaded, deterministic, modern C++17/20 API, production-proven (Horizon Forbidden West, Death Stranding 2), MIT licensed |
| Bullet3 | Considered | Mature and stable but predominantly single-threaded, older API design |
| PhysX (NVIDIA) | Rejected | Proprietary license, GPU path tightly coupled to CUDA rather than Vulkan |
| Havok | Rejected | Commercial/enterprise license, unsuitable for open development |

**Key reasons:**
- **Multi-threaded job system** is essential to overlap physics simulation with Vulkan path-tracing work
- **Deterministic mode** enables lock-step multiplayer replay without re-running full physics on each client
- **Soft body and shape-cast queries** map naturally onto destructible voxel chunks
- Header-friendly C++20 integration

---

### Fluid Simulation — Custom GPU SPH/FLIP via Vulkan Compute

**Selected:** Custom Vulkan Compute Shader implementation (SPH or FLIP method)
**Reference format:** [OpenVDB](https://www.openvdb.org/) / [NanoVDB](https://github.com/AcademySoftwareFoundation/openvdb/tree/master/nanovdb) for sparse volumetric storage
**vcpkg package:** `openvdb` (for data representation only)

| Candidate | Verdict | Notes |
|---|---|---|
| **Custom GPU SPH/FLIP** | ✅ **Selected** | Zero-copy with path tracer, native voxel grid coupling, Vulkan-native |
| OpenVDB | Partial use | Industry standard sparse volumetric format; used for storage and NanoVDB GPU reads, not as a solver |
| SPlisHSPlasH | Rejected | CPU-primary research library; GPU support requires CUDA, incompatible with Vulkan path |
| PhysBAM | Rejected | Academic, outdated toolchain, not game-engine-ready |

**Key reasons:**
- No existing real-time fluid library supports direct Vulkan compute integration
- GPU SPH runs entirely on the same queue as the path tracer, eliminating CPU↔GPU synchronisation overhead
- FLIP (Fluid-Implicit-Particle) offers higher visual fidelity for water; SPH for gas/fire
- OpenVDB/NanoVDB provides the proven data layout for sparse 3-D volumes that flow naturally into the SVO architecture

---

### Particle Simulation — Custom GPU Particle System via Vulkan Compute

**Selected:** Custom Vulkan Compute Shader particle system
**Optional cache/replay:** [Partio](https://github.com/wdas/partio) (Disney) if offline export is needed

| Candidate | Verdict | Notes |
|---|---|---|
| **Custom GPU Compute** | ✅ **Selected** | Native Vulkan, zero-copy with path tracer, direct voxel density read/write |
| Partio (Disney) | Optional | Excellent for particle cache I/O (BGEO, PDB formats); not a simulator |
| Third-party VFX library | Rejected | All mature options (Houdini-side) are CPU-primary or proprietary |

**Key reasons:**
- Particles are a first-class data structure in the voxel simulation loop: they read voxel density, deposit material, and drive fluid advection
- A custom system avoids serialisation across library boundaries
- The particle-to-grid (P2G) and grid-to-particle (G2P) transfers map directly to compute dispatch calls alongside the path-tracing pipeline
- Estimated implementation cost: ~1–2 weeks for a production-quality GPU particle system

---

### ECS (Entity Component System) — EnTT

**Selected:** [EnTT](https://github.com/skypjack/entt)
**vcpkg package:** `entt`

| Candidate | Verdict | Notes |
|---|---|---|
| **EnTT** | ✅ **Selected** | Header-only, C++20, sparse-set storage gives optimal cache locality, 12K+ stars, used in Minecraft Bedrock |
| Flecs | Considered | Excellent query system and built-in pipelines; slightly heavier and less C++20-idiomatic |
| EntityX | Rejected | Archived/unmaintained project |

**Key reasons:**
- **Sparse-set groups** guarantee sequential memory access for tight simulation loops over millions of physics/particle entities
- Header-only: no binary ABI issues across MSVC/GCC/Clang toolchains
- `entt::registry` integrates cleanly with Jolt Physics body handles and the spatial hash grid chunk handles
- Benchmarks consistently show 10–50× iteration throughput advantage over Flecs for dense component queries

---

### Networking — GameNetworkingSockets (Valve)

**Selected:** [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
**vcpkg package:** `gamenetworkingsockets`

| Candidate | Verdict | Notes |
|---|---|---|
| **GameNetworkingSockets** | ✅ **Selected** | Production-proven (Steam), reliable UDP, NAT traversal, P2P + relay hybrid, 9K+ stars |
| yojimbo | Considered | Clean game-focused API, but smaller community and not in vcpkg |
| ENet | Fallback | Excellent lightweight reliable-UDP; lacks built-in NAT traversal |
| Asio | Rejected | Raw sockets only; requires a full game-protocol layer on top |

**Key reasons:**
- **NAT traversal and relay** are mandatory for peer-to-peer voxel world sharing without dedicated server infrastructure
- **Reliable-on-unreliable lanes** let physics state updates use fire-and-forget while chat and chunk data use guaranteed delivery — all on one socket
- Deterministic physics (Jolt) + reliable transport (GNS) enables authoritative rollback networking with minimal bandwidth

---

### Input Management — SDL3 (raw input) + custom action-mapping layer

**Selected:** SDL3 built-in input + thin custom action-mapping layer
**Optional binding library:** [gainput](https://github.com/jkuhlmann/gainput)
**vcpkg package:** SDL3 already included; `gainput` available if needed

| Candidate | Verdict | Notes |
|---|---|---|
| **SDL3 built-in** | ✅ **Selected** | Keyboard, mouse, gamepad (including sensors, rumble, hot-plug), touch — all unified under one event loop we already own |
| gainput | Optional | Cross-platform action-mapping/binding abstraction on top of raw input; vcpkg available; last major release 2018 — limited C++20 support |
| OIS | Rejected | Primarily OGRE-ecosystem, outdated |

**Key reasons:**
- SDL3's `SDL_Event` loop already handles all device types including DualSense/Xbox haptics, touch screens, and IME text input — no additional library needed for raw input
- A lightweight action-mapping layer (e.g., `InputAction` → `SDL_Scancode` binding table, ~200 lines) is engine-specific enough to write in-house; avoids an extra dependency
- Add gainput only if cross-platform controller remapping UI becomes a priority

---

### World Persistence — LMDB (chunks) + SQLite/SQLiteCpp (game state)

**Selected:**
- **[LMDB](https://github.com/LMDB/lmdb)** for voxel chunk on-disk storage
- **[SQLiteCpp](https://github.com/SRombauts/SQLiteCpp)** wrapping SQLite3 for entity/game state

**vcpkg packages:** `lmdb`, `sqlitecpp` (pulls in `sqlite3` automatically)

| Candidate | Verdict | Notes |
|---|---|---|
| **LMDB** | ✅ **Selected (chunks)** | Memory-mapped, ACID, key = uint64 chunk coordinates, value = zstd-compressed SVO bytes; single-process read/write; near-zero copy on reads |
| **SQLite3 + SQLiteCpp** | ✅ **Selected (game state)** | Transactional, zero-config embedded SQL; used for player data, inventories, quest state; R-tree extension available for spatial indexing |
| RocksDB | Rejected | Better for distributed systems; write-amplification overhead undesirable for game chunks |
| LevelDB | Considered | Precursor to RocksDB; simpler, but lacks LMDB's memory-map advantage for large random reads |
| OpenVDB `.vdb` files | Partial | Excellent for authored/exported terrain; too heavy for runtime chunk streaming |

**Persistence strategy:**
```
Chunk key   = morton_encode(chunk_x, chunk_y, chunk_z)  // uint64
Chunk value = zstd_compress(serialise_svo(chunk))        // byte blob
```
- **LMDB** stores the compressed SVO blob — mmap gives O(1) random access without malloc
- **SQLite R-tree** extension (`USING rtree`) provides bounding-box spatial index for persistent entity queries
- On save: dirty chunks are flushed to LMDB; entity/physics state snapshotted to SQLite

---

### Spatial Queries — SQLite R-tree + EnTT views (in-memory)

**Selected:** SQLite3 R-tree extension for persistent spatial index; EnTT views for live in-memory queries
**No additional library required beyond SQLite support already used for persistence**

| Candidate | Verdict | Notes |
|---|---|---|
| **SQLite R-tree extension** | ✅ **Selected (persistent)** | Uses SQLite's R-tree virtual table support (`USING rtree`) to handle bounding-box overlap queries on saved entity state |
| **EnTT views/groups** | ✅ **Selected (in-memory)** | Sparse-set iteration with component filters serves as the live ECS query layer |
| Boost.Geometry | Rejected | Overkill; adds heavy Boost dependency for a feature covered by SQLite and EnTT |

**Key reasons:**
- In-memory spatial queries (nearest entity, frustum cull, physics broad phase) are handled by Jolt Physics (broad phase BVH) and EnTT component views — no extra library needed
- Persistent queries (spawn tables, saved region triggers) naturally fit the SQLite R-tree used for entity persistence

---

### Spatial Audio — Steam Audio

**Selected:** [Steam Audio](https://github.com/ValveSoftware/steam-audio)
**vcpkg package:** `steam-audio`
**Audio I/O backend:** SDL3 audio output (already present) or miniaudio

| Candidate | Verdict | Notes |
|---|---|---|
| **Steam Audio** | ✅ **Selected** | HRTF binaural rendering, physics-based occlusion/reverb via ray casting, path tracing integration possible, Apache-2.0, from Valve (same ecosystem as GNS) |
| SDL3 built-in audio | Partial | Raw PCM playback only — no 3D positioning or reverb |
| SDL3-mixer | Considered | Multi-channel mixing, format decoding — can sit on top of Steam Audio for decoding |
| OpenAL-Soft | Considered | 3D positional audio with EFX reverb; less integrated with a ray-cast world |
| miniaudio | Fallback | Single-header audio I/O; excellent as the device output layer beneath Steam Audio |

**Key reasons:**
- Steam Audio's **occlusion model uses ray casting against the scene geometry** — this maps directly onto the SVO path tracer already in the engine; voxel geometry can be submitted as an occlusion mesh at essentially zero extra cost
- HRTF (Head-Related Transfer Function) provides AAA-quality binaural sound in an open world with minimal CPU cost
- Apache-2.0 license is compatible with MIT/BSD stack
- **SDL3 remains the audio device output backend** (SDL3 → Steam Audio steam-audio pipeline: SDL3 opens device → miniaudio/SDL3 feeds PCM → Steam Audio applies spatialization and convolution reverb)

---

### Math Library — GLM

**Selected:** [GLM](https://glm.g-truc.net) (OpenGL Mathematics)
**vcpkg package:** `glm`

| Candidate | Verdict | Notes |
|---|---|---|
| **GLM** | ✅ **Selected** | Header-only, mirrors GLSL types (`vec3`, `mat4`, `quat`), Vulkan-column-major convention built-in, MIT licensed, 10K+ stars |
| Eigen | Considered | More powerful for linear algebra/physics solvers, but heavier; Jolt ships its own math types anyway |
| Custom | Rejected | No justification given GLM's quality and vcpkg availability |

**Key reasons:**
- GLSL-compatible type names make shader ↔ CPU data layout trivial
- Jolt Physics has its own `JPH::Vec3`/`JPH::Mat44`; conversion helpers to/from `glm` types are one-liners
- `glm::packUnorm4x8` and similar functions are directly useful for voxel material packing

---

### Logging — spdlog

**Selected:** [spdlog](https://github.com/gabime/spdlog)
**vcpkg package:** `spdlog`

| Candidate | Verdict | Notes |
|---|---|---|
| **spdlog** | ✅ **Selected** | Header-only mode or pre-compiled, async sinks, structured log levels, pattern formatting, MIT licensed, industry standard |
| std::print / printf | Rejected | No log levels, no file rotation, no async |
| Boost.Log | Rejected | Heavy dependency for logging alone |

**Key reasons:**
- Async logging sink ensures the hot path (simulation loop, path tracer dispatch) is never stalled by I/O
- Multiple sinks simultaneously (console + rotating file) with zero configuration
- `SPDLOG_TRACE`/`SPDLOG_DEBUG` macros compile to nothing in release builds

---

### Serialization — FlatBuffers

**Selected:** [FlatBuffers](https://google.github.io/flatbuffers/)
**vcpkg package:** `flatbuffers`

| Candidate | Verdict | Notes |
|---|---|---|
| **FlatBuffers** | ✅ **Selected** | Zero-copy deserialisation, memory-mapped access, schema-driven, cross-language (useful for tooling), Apache-2.0; already a transitive dependency of Steam Audio |
| cereal | Considered | Header-only, simple API, but requires full deserialisation into C++ types before use |
| MessagePack | Considered | Compact binary format; no zero-copy |
| Protocol Buffers | Rejected | Already a GNS transitive dependency but overkill as primary serialisation |

**Key reasons:**
- Chunk data, network packets, and save files all benefit from **zero-copy reads** — FlatBuffers buffers can be written directly to LMDB values and read back without parsing
- `flatc` code generator produces typed C++ accessors, eliminating serialisation bugs
- Reuses a dependency already pulled in by `steam-audio`

---

### Compression — zstd

**Selected:** [zstd](https://facebook.github.io/zstd/) (Zstandard)
**vcpkg package:** `zstd`

| Candidate | Verdict | Notes |
|---|---|---|
| **zstd** | ✅ **Selected** | Best ratio/speed tradeoff for structured binary data; dictionary training for voxel chunk patterns; BSD-3 licensed |
| lz4 | Considered | Fastest decompression (~5 GB/s); best for network packets where bandwidth < CPU |
| zlib/deflate | Rejected | Older algorithm; worse ratio and speed than zstd |
| snappy | Rejected | No dictionary support; worse ratio than zstd |

**Key reasons:**
- Voxel chunks have repeated structure (homogeneous regions) that benefits enormously from **zstd dictionary compression** (train a dictionary on a sample of chunks → 2–4× better ratio)
- Used internally by OpenVDB — reusing the same dependency avoids version conflicts
- Level 1 zstd decompresses at ~2 GB/s; sufficient for real-time chunk streaming

---

### Debug UI / Engine Editor — Dear ImGui

**Selected:** [Dear ImGui](https://github.com/ocornut/imgui)
**vcpkg package:** `imgui` with features `vulkan-binding` and `sdl3-binding`

| Candidate | Verdict | Notes |
|---|---|---|
| **Dear ImGui** | ✅ **Selected** | Immediate-mode, zero-retained-state, Vulkan + SDL3 backends both in vcpkg, MIT licensed, industry standard for in-engine tooling |
| Qt | Rejected | Heavyweight, separate event loop incompatible with SDL3 |
| Custom UI | Rejected | High cost for low differentiation |

**Key reasons:**
- Vulkan backend (`imgui[vulkan-binding]`) submits draw calls directly into the main render command buffer
- SDL3 backend (`imgui[sdl3-binding]`) integrates event handling with zero extra work
- Essential from day one: performance overlay, voxel inspector, physics debugger, console

---

### Scripting — Lua + sol2

**Selected:** [Lua](https://www.lua.org/) via [sol2](https://github.com/ThePhD/sol2) C++ bindings
**vcpkg packages:** `lua`, `sol2`

| Candidate | Verdict | Notes |
|---|---|---|
| **Lua + sol2** | ✅ **Selected** | Tiny VM (~250 KB), coroutines native, MIT license, sol2 gives zero-overhead C++ ↔ Lua binding, industry-standard for game modding |
| AngelScript | Considered | Statically typed, C-like syntax; good for gameplay programmers but heavier than Lua |
| ChaiScript | Rejected | Header-only but slower than Lua; limited ecosystem |
| Python | Rejected | Too heavy for an embedded game scripting VM |

**Key reasons:**
- Lua coroutines are ideal for **NPC behaviour trees and quest scripts** — a coroutine can `yield` each frame without callback hell
- sol2's `usertype` binds EnTT entities, Jolt body handles, and voxel grid functions to Lua with minimal boilerplate
- Modding community expects Lua; proven in Factorio, World of Warcraft, Roblox

---

### AI / Navigation — Recast & Detour

**Selected:** [Recast Navigation](https://github.com/recastnavigation/recastnavigation)
**vcpkg package:** `recastnavigation`

| Candidate | Verdict | Notes |
|---|---|---|
| **Recast & Detour** | ✅ **Selected** | Industry standard navmesh (used in Unity, Unreal, Godot), Zlib licensed; Recast builds the mesh, Detour queries it |
| Custom A* on voxel grid | Considered | Simpler but does not handle 3-D environments, crowds, or off-mesh connections |
| PathEngine | Rejected | Commercial license |

**Key reasons:**
- Recast generates navmeshes **from triangle soup or voxel heightfields** — can consume SVO geometry directly
- Detour handles multi-floor 3-D navigation with crowds and dynamic obstacle avoidance
- Tile-based incremental navmesh rebuild matches the chunk-streaming world model

---

### Task Scheduling — Taskflow

**Selected:** [Taskflow](https://github.com/taskflow/taskflow)
**vcpkg package:** `taskflow`

| Candidate | Verdict | Notes |
|---|---|---|
| **Taskflow** | ✅ **Selected** | C++17 task graph with work-stealing, async tasks, CUDA/Vulkan compute integration hooks, MIT licensed |
| Intel TBB | Considered | Mature, high-performance; heavier dependency, Apache-2.0 |
| Jolt's job system | Partial | Excellent for physics work but private API; cannot schedule non-physics tasks |
| std::async / thread pool | Rejected | No dependency graph; deadlock-prone for complex pipelines |

**Key reasons:**
- The main loop is a dependency graph: `[Input] → [Physics] → [SVO update] → [TLAS rebuild] → [Vulkan submit]`
- Taskflow's `tf::Taskflow` expresses this graph declaratively; the work-stealing executor saturates all cores
- Integrates with Jolt's job system by wrapping the physics step as a single Taskflow task
- Header-only mode available for faster iteration

---

## Implementation Roadmap

### Phase 0 — Foundation (Current)
 - [x] SDL3 window creation
 - [ ] Vulkan surface initialisation
 - [ ] Integrate Vulkan device/swapchain setup
 - [ ] Add basic render loop (clear colour, present)
 - [ ] Integrate **spdlog** logging and **Dear ImGui** debug overlay
 - [ ] Integrate **Taskflow** executor for the main loop dependency graph

### Phase 1 — World Representation
 1. [ ] Implement **Spatial Hash Grid** for chunk lifecycle management
 2. [ ] Build **SVO** generator driven by procedural noise (Perlin/Simplex + erosion filter per [blog post](https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html?m=1))
 3. [ ] Implement **DDA Ray-Marcher** in GLSL/HLSL for SVO traversal
 4. [ ] Set up **LMDB** chunk persistence (zstd-compressed FlatBuffers SVO blobs)
 5. [ ] Set up **SQLite/SQLiteCpp** for entity and game state persistence

### Phase 2 — Rendering
 6. [ ] Integrate **TLAS/BLAS** two-level acceleration structure
 7. [ ] Implement path tracer kernel (primary rays → shadow/AO → GI)
 8. [ ] Add **ASVGF temporal re-projection** for denoising

### Phase 3 — Simulation
 9. [ ] Integrate **Jolt Physics** for rigid body + soft body simulation
 10. [ ] Implement GPU **SPH/FLIP fluid simulation** in Vulkan compute
 11. [ ] Implement GPU **particle system** with P2G/G2P voxel coupling

### Phase 4 — Entities & Gameplay
 12. [ ] Integrate **EnTT** ECS; define core component types
 13. [ ] Implement **Dynamic BVH** for character/NPC meshes
 14. [ ] Integrate **Recast & Detour** navmesh for NPC pathfinding
 15. [ ] Add **Lua + sol2** scripting for NPC behaviours, quests, and modding hooks
 16. [ ] Implement SDL3 input action-mapping layer (keyboard/gamepad bindings)

### Phase 5 — Audio
 17. [ ] Integrate **Steam Audio** (HRTF, occlusion via SVO ray casts, convolution reverb)
 18. [ ] Connect SDL3 audio output to Steam Audio PCM pipeline

### Phase 6 — Networking
 19. [ ] Integrate **GameNetworkingSockets**
 20. [ ] Implement chunk streaming and **FlatBuffers + zstd** delta-compression protocol
 21. [ ] Add rollback/reconciliation layer over deterministic Jolt physics
