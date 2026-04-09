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
## Implementation Roadmap
 1. [ ] Implement **Spatial Hash Grid** for chunk lifecycle management.
 2. [ ] Build **SVO** generator for procedural noise (Perlin/Simplex).
 3. [ ] Integrate **TLAS** to manage the intersection between SVO chunks and player meshes.
 4. [ ] Implement **DDA Ray-Marcher** in HLSL/GLSL.
