# UncleRenderer
UncleRenderer is a DirectX 12–based experimental renderer to understand modern engine architecture.

![Pica_pica Scene](screenshots/pica_pica.png)

## Setup
To use this project with ImGui support, you need to initialize the git submodules:
* In Git GUI/Git Sync: Use **SubModule Update** to download the ImGui submodule
* Or via command line: `git submodule update --init --recursive`

## Features

### Core Rendering
* DirectX 12–based renderer
* Forward and Deferred rendering paths
* Render Graph–based pipeline (Barriers, Resource aliasing)
* glTF 2.0 scene and material support

### GPU-Driven Pipeline
* GPU-driven indirect draw and frustum culling
* HZB-based occlusion culling (max 4 mips per dispatch)
* Meshlet-based rendering with per-meshlet culling (early/late visibility, early reject)
* Cluster DAG – hierarchical LOD selection and culling with target pixel error control

### Materials & Lighting
* Physically Based Rendering (GGX)
* Image-Based Lighting (IBL, BRDF LUT)
* Extended PBR material models: Sheen (cloth), Clearcoat, Anisotropy
* Alpha masking and double-sided material support
* Directional shadow mapping
* Ray Traced Shadows (DXR)

### Global Illumination
* Path Tracer (VNDF sampling, configurable bounces, accumulation)
* ReSTIR GI – reservoir-based spatio-temporal importance resampling for indirect lighting
* ReSTIR GI Denoiser (pre-blur, temporal accumulation, SH mip generation, history reconstruction, final blur)

### Reflections & Ambient Occlusion
* Screen-Space Reflections (SSR) – HZB-accelerated ray marching + DXR hardware ray tracing
* SSR Denoiser with edge-aware filtering
* Ground Truth Ambient Occlusion (GTAO)

### Post-Processing
* Temporal Anti-Aliasing (TAA) with motion vectors
* Auto Exposure (eye adaptation)
* PBR Neutral Tonemapping
* Robust Contrast Aware Sharpness (CAS)

### Environment
* Sky / Atmosphere Rendering (Rayleigh / Mie)
* Environment map prefiltering and Spherical Harmonics (SH) irradiance

### Animation
* glTF 2.0 skeletal animation
* GPU skinning via compute shader

### Tools & Debug
* Asynchronous scene and texture loading (Task system)
* QEM-based LOD mesh generation
* Object ID rendering (GPU instance picking)
* GPU debug system (print, debug boxes/lines, statistics)
* ImGui-based debug and profiling UI (D3D12 Timestamp Query)
