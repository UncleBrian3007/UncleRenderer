# UncleRenderer
UncleRenderer is a DirectX 12–based experimental renderer to undestand modern engine architecture.

![Pica_pica Scene](screenshots/pica_pica.png)

## Features
* DirectX 12–based renderer
* Forward and Deferred rendering paths
* GPU-driven indirect draw and frustum culling
* HZB-based occlusion culling (max 4 mips per dispatch)
* Physically Based Rendering (GGX)
* Image-Based Lighting (IBL, BRDF LUT)
* Directional shadow mapping
* Render Graph–based pipeline (Barriers, Resource aliasing)
* glTF 2.0 scene and material support
* Asynchronous scene and texture loading (Task system)
* Sky / Atmosphere Rendering (Rayleigh / Mie)
* HDR Tonemapping Pass
* ImGui-based debug and profiling UI (D3D12 Timestamp Query)
