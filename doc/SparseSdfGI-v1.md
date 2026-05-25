# SparseSdfGI v1

This page records the original v1 prototype. The current v2 plan and follow-up work are tracked in `doc/SparseSdfGI-v2.md`.

SparseSdfGI is an original UncleRenderer runtime sparse SDF GI experiment inspired by AMD FidelityFX Brixelizer and Brixelizer GI. The implementation is native to UncleRenderer and does not copy FidelityFX SDK source, shader code, or data tables.

## Goals

- Build a runtime scene-space SDF from regular static mesh triangles.
- Use a Brixelizer-style indirection layout: a 64^3 cascade brick map points at 8^3 bricks inside a 512^3 SDF atlas.
- Start with a dense single-cascade mapping where `brickId == linear cascade cell index`.
- Provide debug ray traversal before optimizing sparse allocation, clipmap scrolling, or AABB-tree skipping.

## v1 Pipeline

```text
SparseSdfGI SDF Clear
  -> initialize dense cascade brick map
  -> clear SDF atlas to empty distance

SparseSdfGI Voxelize Model N
  -> static regular mesh triangles
  -> approximate normalized narrow-band distance in SDF atlas

SparseSdfGI Trace
  -> debug ray traversal or occlusion-based diffuse placeholder output
  -> optional composite as indirect diffuse
```

The first implementation prioritizes correctness and visibility over performance. The diffuse path is a placeholder that samples a cosine hemisphere direction and darkens rays that hit the SDF; it is closer to SDF-guided ambient occlusion than full GI until hit radiance or a radiance cache is added. It skips skinned meshes, ClusterDAG packed geometry, sparse brick allocation, static/dynamic merging, Eikonal solving, and radiance/specular caches.

## References / Attribution

- AMD FidelityFX SDK, Brixelizer / Brixelizer GI
- Copyright (C) 2024 Advanced Micro Devices, Inc.
- Licensed under the MIT License

If SDK code, HLSL, constants, or substantial portions are copied later, add a `ThirdPartyNotices.md` entry with the full MIT license text and the copied scope.
