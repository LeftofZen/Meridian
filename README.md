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
Traditional meshes are unsuitable for infinite, editable terrain. We use an **SVO** to represent the world. The world heightmap is procudrally generated in chunks using the methods outlined in this blog post: https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html?m=1.
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

## Implementation Roadmap

### Phase 0 — Foundation (Current)
 - [x] SDL3 window + Vulkan surface initialisation
 - [ ] Integrate Vulkan device/swapchain setup
 - [ ] Add basic render loop (clear colour, present)

### Phase 1 — World Representation
 1. [ ] Implement **Spatial Hash Grid** for chunk lifecycle management
 2. [ ] Build **SVO** generator driven by procedural noise (Perlin/Simplex + erosion filter per [blog post](https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html?m=1))
 3. [ ] Implement **DDA Ray-Marcher** in GLSL/HLSL for SVO traversal

### Phase 2 — Rendering
 4. [ ] Integrate **TLAS/BLAS** two-level acceleration structure
 5. [ ] Implement path tracer kernel (primary rays → shadow/AO → GI)
 6. [ ] Add **ASVGF temporal re-projection** for denoising

### Phase 3 — Simulation
 7. [ ] Integrate **Jolt Physics** for rigid body + soft body simulation
 8. [ ] Implement GPU **SPH/FLIP fluid simulation** in Vulkan compute
 9. [ ] Implement GPU **particle system** with P2G/G2P voxel coupling

### Phase 4 — Entities & Gameplay
 10. [ ] Integrate **EnTT** ECS; define core component types
 11. [ ] Implement **Dynamic BVH** for character/NPC meshes
 12. [ ] Add NPC AI and player controller

### Phase 5 — Networking
 13. [ ] Integrate **GameNetworkingSockets**
 14. [ ] Implement chunk streaming and delta-compression protocol
 15. [ ] Add rollback/reconciliation layer over deterministic Jolt physics
