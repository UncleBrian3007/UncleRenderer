#include "DeferredRenderer.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "TextureLoader.h"
#include "RenderGraph.h"
#include "Deferred/DeferredPassContext.h"
#include "Deferred/DeferredFrameOrchestrator.h"
#include "Deferred/DeferredVisibilityPasses.h"
#include "Deferred/DeferredGeometryPasses.h"
#include "Deferred/DeferredLightingPasses.h"
#include "Deferred/Gtao.h"
#include "Deferred/RayTracingShadow.h"
#include "Deferred/Ssr.h"
#include "Deferred/RestirGI.h"
#include "Deferred/AutoExposure.h"
#include "Deferred/Cas.h"
#include "Deferred/Taa.h"
#include "Deferred/Tonemap.h"
#include "Deferred/PathTracing.h"
#include "Deferred/DeferredPostProcessPasses.h"
#include "Deferred/DeferredResourceImporter.h"
#include "../Scene/GltfLoader.h"
#include "../Scene/Camera.h"
#include "../Scene/Mesh.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12CommandQueue.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../Core/RendererConfig.h"
#include <d3dx12.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <array>
#include <sstream>

using Microsoft::WRL::ComPtr;

FDeferredRenderer::FDeferredRenderer()
    : FrameOrchestrator(std::make_unique<FDeferredFrameOrchestrator>())
    , VisibilityPasses(std::make_unique<FDeferredVisibilityPasses>())
    , GeometryPasses(std::make_unique<FDeferredGeometryPasses>())
    , LightingPasses(std::make_unique<FDeferredLightingPasses>())
    , Gtao(std::make_unique<FGtao>())
    , RayTracingShadow(std::make_unique<FRayTracingShadow>())
    , Ssr(std::make_unique<FSsr>())
    , RestirGI(std::make_unique<FRestirGI>())
    , RestirGIDenoiser(std::make_unique<FRestirGIDenoiser>())
    , PathTracing(std::make_unique<FPathTracing>())
    , AutoExposure(std::make_unique<FAutoExposure>())
    , Cas(std::make_unique<FCas>())
    , Taa(std::make_unique<FTaa>())
    , Tonemap(std::make_unique<FTonemap>())
    , PostProcessPasses(std::make_unique<FDeferredPostProcessPasses>())
    , ResourceImporter(std::make_unique<FDeferredResourceImporter>())
{
}

const DXGI_FORMAT FDeferredRenderer::GBufferFormats[4] =
{
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
};

void FDeferredRenderer::SetTonemapEnabled(bool bEnabled)
{
    if (Tonemap)
    {
        Tonemap->SetTonemapEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsTonemapEnabled() const
{
    return Tonemap && Tonemap->IsTonemapEnabled();
}

void FDeferredRenderer::SetTonemapExposure(float Exposure)
{
    if (Tonemap)
    {
        Tonemap->SetTonemapExposure(Exposure);
    }
}

float FDeferredRenderer::GetTonemapExposure() const
{
    return Tonemap ? Tonemap->GetTonemapExposure() : 0.0f;
}

void FDeferredRenderer::SetTonemapWhitePoint(float WhitePoint)
{
    if (Tonemap)
    {
        Tonemap->SetTonemapWhitePoint(WhitePoint);
    }
}

float FDeferredRenderer::GetTonemapWhitePoint() const
{
    return Tonemap ? Tonemap->GetTonemapWhitePoint() : 0.0f;
}

void FDeferredRenderer::SetTonemapGamma(float Gamma)
{
    if (Tonemap)
    {
        Tonemap->SetTonemapGamma(Gamma);
    }
}

float FDeferredRenderer::GetTonemapGamma() const
{
    return Tonemap ? Tonemap->GetTonemapGamma() : 0.0f;
}

void FDeferredRenderer::SetCasEnabled(bool bEnabled)
{
    if (Cas)
    {
        Cas->SetEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsCasEnabled() const
{
    return Cas && Cas->IsEnabled();
}

void FDeferredRenderer::SetCasSharpness(float Sharpness)
{
    if (Cas)
    {
        Cas->SetSharpness(Sharpness);
    }
}

float FDeferredRenderer::GetCasSharpness() const
{
    return Cas ? Cas->GetSharpness() : 0.0f;
}

void FDeferredRenderer::SetAutoExposureEnabled(bool bEnabled)
{
    if (AutoExposure)
    {
        AutoExposure->SetEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsAutoExposureEnabled() const
{
    return AutoExposure && AutoExposure->IsEnabled();
}

void FDeferredRenderer::SetAutoExposureKey(float Key)
{
    if (AutoExposure)
    {
        AutoExposure->SetKey(Key);
    }
}

float FDeferredRenderer::GetAutoExposureKey() const
{
    return AutoExposure ? AutoExposure->GetKey() : 0.0f;
}

void FDeferredRenderer::SetAutoExposureMin(float MinExposure)
{
    if (AutoExposure)
    {
        AutoExposure->SetMinExposure(MinExposure);
    }
}

float FDeferredRenderer::GetAutoExposureMin() const
{
    return AutoExposure ? AutoExposure->GetMinExposure() : 0.0f;
}

void FDeferredRenderer::SetAutoExposureMax(float MaxExposure)
{
    if (AutoExposure)
    {
        AutoExposure->SetMaxExposure(MaxExposure);
    }
}

float FDeferredRenderer::GetAutoExposureMax() const
{
    return AutoExposure ? AutoExposure->GetMaxExposure() : 0.0f;
}

void FDeferredRenderer::SetAutoExposureSpeedUp(float Speed)
{
    if (AutoExposure)
    {
        AutoExposure->SetSpeedUp(Speed);
    }
}

float FDeferredRenderer::GetAutoExposureSpeedUp() const
{
    return AutoExposure ? AutoExposure->GetSpeedUp() : 0.0f;
}

void FDeferredRenderer::SetAutoExposureSpeedDown(float Speed)
{
    if (AutoExposure)
    {
        AutoExposure->SetSpeedDown(Speed);
    }
}

float FDeferredRenderer::GetAutoExposureSpeedDown() const
{
    return AutoExposure ? AutoExposure->GetSpeedDown() : 0.0f;
}

void FDeferredRenderer::SetTaaEnabled(bool bEnabled)
{
    if (Taa)
    {
        Taa->SetEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsTaaEnabled() const
{
    return Taa && Taa->IsEnabled();
}

void FDeferredRenderer::SetTaaHistoryWeight(float Weight)
{
    if (Taa)
    {
        Taa->SetHistoryWeight(Weight);
    }
}

float FDeferredRenderer::GetTaaHistoryWeight() const
{
    return Taa ? Taa->GetHistoryWeight() : 0.0f;
}

void FDeferredRenderer::SetSsrSwEnabled(bool bEnabled)
{
    if (Ssr)
    {
        Ssr->SetSwEnabled(bEnabled);
    }
}

void FDeferredRenderer::SetSsrHwEnabled(bool bEnabled)
{
    if (Ssr)
    {
        Ssr->SetHwEnabled(bEnabled);
    }
}

void FDeferredRenderer::SetSsrHzbEnabled(bool bEnabled)
{
    if (Ssr)
    {
        Ssr->SetHzbEnabled(bEnabled);
    }
}

void FDeferredRenderer::SetSsrRefineEnabled(bool bEnabled)
{
    if (Ssr)
    {
        Ssr->SetRefineEnabled(bEnabled);
    }
}

void FDeferredRenderer::SetSsrDenoiseEnabled(bool bEnabled)
{
    if (Ssr)
    {
        Ssr->SetDenoiseEnabled(bEnabled);
    }
}

void FDeferredRenderer::SetSsrMode(ESSRMode Mode)
{
    if (Ssr)
    {
        Ssr->SetMode(Mode);
    }
}

void FDeferredRenderer::SetSsrSamplesPerQuad(uint32_t Samples)
{
    if (Ssr)
    {
        Ssr->SetSamplesPerQuad(Samples);
    }
}

bool FDeferredRenderer::IsSsrSwEnabled() const
{
    return Ssr && Ssr->IsSwEnabled();
}

bool FDeferredRenderer::IsSsrHwEnabled() const
{
    return Ssr && Ssr->IsHwEnabled();
}

void FDeferredRenderer::SetSsrMaxSteps(uint32_t Steps)
{
    if (Ssr)
    {
        Ssr->SetMaxSteps(Steps);
    }
}

uint32_t FDeferredRenderer::GetSsrMaxSteps() const
{
    return Ssr ? Ssr->GetMaxSteps() : 0u;
}

void FDeferredRenderer::SetSsrMaxDistance(float Distance)
{
    if (Ssr)
    {
        Ssr->SetMaxDistance(Distance);
    }
}

float FDeferredRenderer::GetSsrMaxDistance() const
{
    return Ssr ? Ssr->GetMaxDistance() : 0.0f;
}

void FDeferredRenderer::SetSsrThickness(float Thickness)
{
    if (Ssr)
    {
        Ssr->SetThickness(Thickness);
    }
}

float FDeferredRenderer::GetSsrThickness() const
{
    return Ssr ? Ssr->GetThickness() : 0.0f;
}

void FDeferredRenderer::SetSsrStride(float Stride)
{
    if (Ssr)
    {
        Ssr->SetStride(Stride);
    }
}

float FDeferredRenderer::GetSsrStride() const
{
    return Ssr ? Ssr->GetStride() : 0.0f;
}

void FDeferredRenderer::SetSsrRoughnessCutoff(float Cutoff)
{
    if (Ssr)
    {
        Ssr->SetRoughnessCutoff(Cutoff);
    }
}

float FDeferredRenderer::GetSsrRoughnessCutoff() const
{
    return Ssr ? Ssr->GetRoughnessCutoff() : 0.0f;
}

void FDeferredRenderer::SetSsrIntensity(float Intensity)
{
    if (Ssr)
    {
        Ssr->SetIntensity(Intensity);
    }
}

float FDeferredRenderer::GetSsrIntensity() const
{
    return Ssr ? Ssr->GetIntensity() : 0.0f;
}

void FDeferredRenderer::SetRestirGIEnabled(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGIEnabled() const
{
    return RestirGI && RestirGI->IsEnabled();
}

void FDeferredRenderer::SetRestirGIDenoiserEnabled(bool bEnabled)
{
    const bool bCurrentEnabled = RestirGIDenoiser && RestirGIDenoiser->IsEnabled();
    if (bCurrentEnabled != bEnabled)
    {
        if (RestirGIDenoiser)
        {
            RestirGIDenoiser->SetEnabled(bEnabled);
        }
        InvalidateRestirGiDenoiserHistory();
    }
}

bool FDeferredRenderer::IsRestirGIDenoiserEnabled() const
{
    return RestirGIDenoiser && RestirGIDenoiser->IsEnabled();
}

void FDeferredRenderer::SetRestirGIFreezeDenoiserHistoryResetPeriod(uint32_t InPeriod)
{
    if (RestirGIDenoiser)
    {
        RestirGIDenoiser->SetFreezeHistoryResetPeriod(InPeriod);
    }
}

uint32_t FDeferredRenderer::GetRestirGIFreezeDenoiserHistoryResetPeriod() const
{
    return RestirGIDenoiser ? RestirGIDenoiser->GetFreezeHistoryResetPeriod() : 0u;
}

void FDeferredRenderer::SetRestirGISamplesPerPixel(uint32_t Samples)
{
    if (RestirGI)
    {
        RestirGI->SetSamplesPerPixel(Samples);
    }
}

uint32_t FDeferredRenderer::GetRestirGISamplesPerPixel() const
{
    return RestirGI ? RestirGI->GetSamplesPerPixel() : 0u;
}

void FDeferredRenderer::SetRestirGIIntensity(float Intensity)
{
    if (RestirGI)
    {
        RestirGI->SetIntensity(Intensity);
    }
}

float FDeferredRenderer::GetRestirGIIntensity() const
{
    return RestirGI ? RestirGI->GetIntensity() : 0.0f;
}

void FDeferredRenderer::SetRestirGIShowOnly(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetShowOnly(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGIShowOnly() const
{
    return RestirGI && RestirGI->IsShowOnly();
}

void FDeferredRenderer::SetRestirGITemporalReuseEnabled(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetTemporalReuseEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGITemporalReuseEnabled() const
{
    return RestirGI && RestirGI->IsTemporalReuseEnabled();
}

void FDeferredRenderer::SetRestirGISpatialReuseEnabled(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetSpatialReuseEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGISpatialReuseEnabled() const
{
    return RestirGI && RestirGI->IsSpatialReuseEnabled();
}

void FDeferredRenderer::SetRestirGITemporalAdditionalScale(float Value)
{
    if (RestirGI)
    {
        RestirGI->SetTemporalAdditionalScale(Value);
    }
}

float FDeferredRenderer::GetRestirGITemporalAdditionalScale() const
{
    return RestirGI ? RestirGI->GetTemporalAdditionalScale() : 0.0f;
}

void FDeferredRenderer::SetRestirGISpatialAdditionalScale(float Value)
{
    if (RestirGI)
    {
        RestirGI->SetSpatialAdditionalScale(Value);
    }
}

float FDeferredRenderer::GetRestirGISpatialAdditionalScale() const
{
    return RestirGI ? RestirGI->GetSpatialAdditionalScale() : 0.0f;
}

void FDeferredRenderer::SetRestirGIResolveMinDenominator(float Value)
{
    if (RestirGI)
    {
        RestirGI->SetResolveMinDenominator(Value);
    }
}

float FDeferredRenderer::GetRestirGIResolveMinDenominator() const
{
    return RestirGI ? RestirGI->GetResolveMinDenominator() : 0.0f;
}

void FDeferredRenderer::SetRestirGIResolveMaxNormalization(float Value)
{
    if (RestirGI)
    {
        RestirGI->SetResolveMaxNormalization(Value);
    }
}

float FDeferredRenderer::GetRestirGIResolveMaxNormalization() const
{
    return RestirGI ? RestirGI->GetResolveMaxNormalization() : 0.0f;
}

void FDeferredRenderer::SetRestirGIResolveLowSampleBoostGuard(float Value)
{
    if (RestirGI)
    {
        RestirGI->SetResolveLowSampleBoostGuard(Value);
    }
}

float FDeferredRenderer::GetRestirGIResolveLowSampleBoostGuard() const
{
    return RestirGI ? RestirGI->GetResolveLowSampleBoostGuard() : 0.0f;
}

void FDeferredRenderer::SetRestirGIResolveUseConfidence(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetResolveUseConfidence(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGIResolveUseConfidence() const
{
    return RestirGI && RestirGI->IsResolveUseConfidence();
}

void FDeferredRenderer::SetRestirGIUseVisibility(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetUseVisibility(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGIUseVisibility() const
{
    return RestirGI && RestirGI->IsUseVisibility();
}

void FDeferredRenderer::SetRestirGIUseBrdf(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetUseBrdf(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGIUseBrdf() const
{
    return RestirGI && RestirGI->IsUseBrdf();
}

void FDeferredRenderer::SetRestirGIUseHistoryIndirect(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetUseHistoryIndirect(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGIUseHistoryIndirect() const
{
    return RestirGI && RestirGI->IsUseHistoryIndirect();
}

void FDeferredRenderer::SetRestirGIRandomMode(ERestirGIRandomMode Mode)
{
    if (!RestirGI)
    {
        return;
    }

    if (RestirGI->GetRandomMode() != Mode)
    {
        RestirGI->SetRandomMode(Mode);
        RestirGI->InvalidateReservoirHistory();
        InvalidateRestirGiDenoiserHistory();
    }
}

ERestirGIRandomMode FDeferredRenderer::GetRestirGIRandomMode() const
{
    return RestirGI ? RestirGI->GetRandomMode() : ERestirGIRandomMode::BlueNoiseSobol;
}

void FDeferredRenderer::SetRestirGIDebugRayEnabled(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetDebugRayEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsRestirGIDebugRayEnabled() const
{
    return RestirGI && RestirGI->IsDebugRayEnabled();
}

void FDeferredRenderer::SetRestirGIDebugPixel(uint32_t X, uint32_t Y)
{
    if (RestirGI)
    {
        RestirGI->SetDebugPixel(X, Y);
    }
}

void FDeferredRenderer::SetRestirGIFreezeFrame(bool bEnabled)
{
    if (RestirGI)
    {
        RestirGI->SetFreezeFrame(bEnabled, GetFrameNumber());
    }
}

bool FDeferredRenderer::IsRestirGIFreezeFrame() const
{
    return RestirGI && RestirGI->IsFreezeFrame();
}

uint32_t FDeferredRenderer::GetRestirGIFrozenSequenceFrame() const
{
    return RestirGI ? RestirGI->GetFrozenSequenceFrame() : 0u;
}

uint64_t FDeferredRenderer::GetRestirGIFreezeStartFrameNumber() const
{
    return RestirGI ? RestirGI->GetFreezeStartFrameNumber() : 0u;
}

void FDeferredRenderer::StepRestirGIFreezeFrame()
{
    if (RestirGI)
    {
        RestirGI->StepFreezeFrame();
    }
}

void FDeferredRenderer::SetPathTracingAccumulationEnabled(bool bEnabled)
{
    if (PathTracing)
    {
        PathTracing->SetAccumulationEnabled(bEnabled);
    }
}

bool FDeferredRenderer::IsPathTracingAccumulationEnabled() const
{
    return PathTracing && PathTracing->IsAccumulationEnabled();
}

void FDeferredRenderer::SetPathTracingMaxBounces(uint32_t MaxBounces)
{
    if (PathTracing)
    {
        PathTracing->SetMaxBounces(MaxBounces);
    }
}

uint32_t FDeferredRenderer::GetPathTracingMaxBounces() const
{
    return PathTracing ? PathTracing->GetMaxBounces() : 0u;
}

void FDeferredRenderer::SetPathTracingVndfEnabled(bool bEnabled)
{
    bPathTracingUseVndf = bEnabled;
    if (PathTracing)
    {
        PathTracing->ResetAccumulation();
    }
}

void FDeferredRenderer::SetPathTracingDebugMode(int Mode)
{
    if (PathTracing)
    {
        PathTracing->SetDebugMode(Mode);
    }
}

int FDeferredRenderer::GetPathTracingDebugMode() const
{
    return PathTracing ? PathTracing->GetDebugMode() : 0;
}

DXGI_FORMAT FDeferredRenderer::ResolveRestirGiRadianceFormat(FDX12Device* Device) const
{
    return RestirGI ? RestirGI->ResolveRadianceFormat(Device) : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool FDeferredRenderer::Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config)
{
    if (Device == nullptr)
    {
        LogError("Deferred renderer initialization failed: device is null");
        return false;
    }

    this->Device = Device;

    LogInfo("Deferred renderer initialization started");

    this->BackBufferFormat = BackBufferFormat;

    ApplyRendererConfig(Config);

    InitializeCommonSettings(Width, Height, Config);

    if (!InitializePipelineDomains(Device, BackBufferFormat))
    {
        return false;
    }

    if (!InitializeFrameResources(Device, Width, Height, Config))
    {
        return false;
    }

    if (!InitializeSceneResources(Device, BackBufferFormat, Config))
    {
        return false;
    }

    LogInfo("Deferred renderer initialization completed");
    return true;
}

void FDeferredRenderer::ApplyRendererConfig(const FRendererConfig& Config)
{
    SetTonemapEnabled(Config.bEnableTonemap);
    SetTonemapExposure(Config.TonemapExposure);
    SetTonemapWhitePoint(Config.TonemapWhitePoint);
    SetTonemapGamma(Config.TonemapGamma);
    SetCasEnabled(Config.bEnableCas);
    SetCasSharpness(Config.CasSharpness);
    SetAutoExposureEnabled(Config.bEnableAutoExposure);
    SetAutoExposureKey(Config.AutoExposureKey);
    SetAutoExposureMin(Config.AutoExposureMin);
    SetAutoExposureMax(Config.AutoExposureMax);
    SetAutoExposureSpeedUp(Config.AutoExposureSpeedUp);
    SetAutoExposureSpeedDown(Config.AutoExposureSpeedDown);
    SetTaaEnabled(Config.bEnableTAA);
    SetTaaHistoryWeight(Config.TaaHistoryWeight);
    bHZBEnabled = Config.bEnableHZB;
    bHZBReady = false;
    bEnablePbrResearch = Config.bEnablePbrResearch;
    if (PathTracing)
    {
        PathTracing->SetAccumulationEnabled(Config.bEnablePathTracingAccumulation);
        PathTracing->SetMaxBounces(Config.PathTracingMaxBounces);
    }
    if (Ssr)
    {
        Ssr->SetSwEnabled(Config.bEnableSsrSw);
        Ssr->SetHwEnabled(Config.bEnableSsrHw);
        Ssr->SetHzbEnabled(Config.bEnableSsrHzb);
        Ssr->SetRefineEnabled(Config.bEnableSsrRefine);
        Ssr->SetDenoiseEnabled(Config.bEnableSsrDenoise);
    }
    if (RestirGIDenoiser)
    {
        RestirGIDenoiser->SetEnabled(Config.bEnableRestirGIDenoiser);
    }
    if (Ssr)
    {
        Ssr->SetMaxSteps(Config.SsrMaxSteps);
        Ssr->SetMaxDistance(Config.SsrMaxDistance);
        Ssr->SetThickness(Config.SsrThickness);
        Ssr->SetStride(Config.SsrStride);
        Ssr->SetRoughnessCutoff(Config.SsrRoughnessCutoff);
        Ssr->SetIntensity(Config.SsrIntensity);
    }
    if (RestirGI)
    {
        RestirGI->SetEnabled(Config.bEnableRestirGI);
        RestirGI->SetSamplesPerPixel(std::clamp(Config.RestirGISamplesPerPixel, 1u, 32u));
        RestirGI->SetIntensity((std::max)(0.0f, Config.RestirGIIntensity));
        RestirGI->SetRayLength((std::max)(0.1f, Config.RestirGIRayLength));
        RestirGI->SetClampThreshold((std::max)(0.1f, Config.RestirGIClamp));
        RestirGI->SetTemporalReuseEnabled(Config.bEnableRestirGITemporalReuse);
        RestirGI->SetSpatialReuseEnabled(Config.bEnableRestirGISpatialReuse);
        RestirGI->SetTemporalAdditionalScale(std::clamp(Config.RestirGITemporalAdditionalScale, 0.0f, 1.0f));
        RestirGI->SetSpatialAdditionalScale(std::clamp(Config.RestirGISpatialAdditionalScale, 0.0f, 1.0f));
        RestirGI->SetResolveMinDenominator((std::max)(Config.RestirGIResolveMinDenominator, 1e-6f));
        RestirGI->SetResolveMaxNormalization((std::max)(Config.RestirGIResolveMaxNormalization, 1.0f));
        RestirGI->SetResolveLowSampleBoostGuard(std::clamp(Config.RestirGIResolveLowSampleBoostGuard, 0.0f, 1.0f));
        RestirGI->SetResolveUseConfidence(Config.bRestirGIResolveUseConfidence);
        RestirGI->SetMaxHistoryFrames(std::clamp(Config.RestirGIMaxHistoryFrames, 1u, 16u));
        RestirGI->SetUseVisibility(Config.bRestirGIUseVisibility);
        RestirGI->SetUseBrdf(Config.bRestirGIUseBrdf);
        RestirGI->SetUseHistoryIndirect(Config.bRestirGIUseHistoryIndirect);
        RestirGI->SetRandomMode(Config.RestirGIRandomMode);
    }
    if (Ssr)
    {
        Ssr->SetMode(Config.SsrMode);
        Ssr->SetSamplesPerQuad(Config.SsrSamplesPerQuad);
    }
}

bool FDeferredRenderer::InitializePipelineDomains(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    if (!CreateRayTracingPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: ray tracing pipeline creation failed");
        return false;
    }
    if (!CreateSkinningPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: skinning pipeline creation failed");
        return false;
    }
    if (!CreateEnvironmentBuildPipelines(Device))
    {
        LogError("Deferred renderer initialization failed: environment build pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer geometry domain pipelines...");
    if (!GeometryPasses->InitializePipelines(*this, Device, LightingBufferFormat))
    {
        LogError("Deferred renderer initialization failed: geometry domain pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer lighting domain pipelines...");
    if (!LightingPasses->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: lighting domain pipeline creation failed");
        return false;
    }

    if (Gtao && !Gtao->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: GTAO pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer path tracing pipelines...");
    if (PathTracing && !PathTracing->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: path tracing pipeline creation failed");
        return false;
    }

    if (RestirGI && !RestirGI->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI pipeline creation failed");
        return false;
    }

    if (RestirGIDenoiser && !RestirGIDenoiser->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI denoiser pipeline creation failed");
        return false;
    }

    if (Ssr && !Ssr->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: SSR pipeline creation failed");
        return false;
    }

    if (AutoExposure && !AutoExposure->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: auto exposure pipeline creation failed");
        return false;
    }

    if (Cas && !Cas->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: CAS pipeline creation failed");
        return false;
    }

    if (Tonemap && !Tonemap->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: tonemap pipeline creation failed");
        return false;
    }

    if (Taa && !Taa->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: TAA pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer post-process pipelines...");
    if (!PostProcessPasses->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: post-process pipeline creation failed");
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeFrameResources(FDX12Device* Device, uint32_t Width, uint32_t Height, const FRendererConfig& Config)
{
    TextureLoader = std::make_unique<FTextureLoader>(Device);

    if (!TextureLoader->LoadOrSolidColor(L"", 0xffffffff, NullTexture))
    {
        LogError("Deferred renderer initialization failed: null texture creation failed");
        return false;
    }

    if (NullTexture)
    {
        NullTexture->SetName(L"NullTexture");
    }

    if (!GeometryPasses->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: geometry domain resource creation failed");
        return false;
    }

    if (!PostProcessPasses->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: post-process resource creation failed");
        return false;
    }

    if (Taa && !Taa->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: TAA resource creation failed");
        return false;
    }

    if (AutoExposure && !AutoExposure->InitializeResources(*this, Device))
    {
        LogError("Deferred renderer initialization failed: auto exposure resource creation failed");
        return false;
    }

    if (Cas && !Cas->InitializeResources(*this, Device))
    {
        LogError("Deferred renderer initialization failed: CAS resource creation failed");
        return false;
    }

    if (Tonemap && !Tonemap->InitializeResources(*this, Device))
    {
        LogError("Deferred renderer initialization failed: tonemap resource creation failed");
        return false;
    }

    if (PathTracing && !PathTracing->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: path tracing resource creation failed");
        return false;
    }

    if (RestirGI && !RestirGI->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI resource creation failed");
        return false;
    }

    if (RestirGIDenoiser && !RestirGIDenoiser->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI denoiser resource creation failed");
        return false;
    }

    if (Ssr && !Ssr->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: SSR resource creation failed");
        return false;
    }

    if (!LightingPasses->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: lighting domain resource creation failed");
        return false;
    }

    if (Gtao && !Gtao->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: GTAO resource creation failed");
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeSceneResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config)
{
    if (!InitializeSceneModelResources(Device, Config))
    {
        return false;
    }

    if (!InitializeEnvironmentAndDescriptorResources(Device, Config))
    {
        return false;
    }

    if (!VisibilityPasses->InitializeResources(*this, Device))
    {
        LogWarning("Deferred renderer GPU-driven resources creation failed; fallback to CPU-driven draws.");
    }

    if (!InitializeSkyResources(Device))
    {
        return false;
    }

    if (!InitializeGpuDebugResources(Device, BackBufferFormat))
    {
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeSceneModelResources(FDX12Device* Device, const FRendererConfig& Config)
{
    const std::wstring SceneFilePath = Config.SceneFile.empty() ? L"Assets/Scenes/Scene.json" : Config.SceneFile;
    if (!RendererUtils::CreateSceneModelsFromJson(Device, SceneFilePath, SceneModels, SceneCenter, SceneRadius, &GltfScenes))
    {
        LogError("scene JSON could not be loaded.");
        return false;
    }

    if (!CreateSkinnedPositionBuffers())
    {
        LogError("Deferred renderer initialization failed: skinned position buffer creation failed");
        return false;
    }

    GltfScenePoses.resize(GltfScenes.size());
    GltfSceneTimes.assign(GltfScenes.size(), 0.0f);
    for (size_t Index = 0; Index < GltfScenes.size(); ++Index)
    {
        InitializeGltfAnimationPose(GltfScenes[Index], GltfScenePoses[Index]);
    }

    SceneWorldMatrix = SceneModels.front().WorldMatrix;
    for (FSceneModelResource& Model : SceneModels)
    {
        Model.PreviousWorldMatrix = Model.WorldMatrix;
        Model.bHasPreviousWorldMatrix = false;
    }

    SceneConstantBufferStride = (sizeof(FSceneConstants) + 255ULL) & ~255ULL;
    const uint64_t ConstantBufferSize = SceneConstantBufferStride * (std::max<uint64_t>(1, SceneModels.size()));

    if (!CreateSceneConstantBuffersPerFrame(Device, ConstantBufferSize))
    {
        LogError("Deferred renderer initialization failed: constant buffer creation failed");
        return false;
    }
    if (!CreateCullingConstantBuffersPerFrame(Device))
    {
        LogError("Deferred renderer initialization failed: culling constant buffer creation failed");
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeEnvironmentAndDescriptorResources(FDX12Device* Device, const FRendererConfig& Config)
{
    if (!BuildEnvironmentFromEquirect(Config))
    {
        LogError("Deferred renderer initialization failed: environment build requires R11G11B10 typed UAV support");
        return false;
    }
    if (EnvironmentCubeTexture)
    {
        EnvironmentCubeTexture->SetName(L"EnvironmentCube");
    }

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/PreintegratedGF.dds", BrdfLutTexture))
    {
        LogError("Deferred renderer initialization failed: BRDF LUT texture loading failed");
        return false;
    }
    if (BrdfLutTexture)
    {
        BrdfLutTexture->SetName(L"BrdfLut");
    }

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/BlueNoise/sobol_256_4d.png", BlueNoiseSobolTexture))
    {
        LogError("Deferred renderer initialization failed: blue noise sobol texture loading failed");
        return false;
    }
    if (BlueNoiseSobolTexture)
    {
        BlueNoiseSobolTexture->SetName(L"BlueNoiseSobol");
    }

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/BlueNoise/scrambling_ranking_128x128_2d_1spp.png", BlueNoiseScramblingRanking1SPPTexture))
    {
        LogError("Deferred renderer initialization failed: blue noise scrambling/ranking texture loading failed");
        return false;
    }
    if (BlueNoiseScramblingRanking1SPPTexture)
    {
        BlueNoiseScramblingRanking1SPPTexture->SetName(L"BlueNoiseScramblingRanking1SPP");
    }

    if (EnvironmentCubeTexture)
    {
        const D3D12_RESOURCE_DESC EnvDesc = EnvironmentCubeTexture->GetDesc();
        EnvironmentMipCount = static_cast<float>((std::max)(1u, static_cast<uint32_t>(EnvDesc.MipLevels)));
    }

    if (!CreateSceneTextures(Device, SceneModels))
    {
        LogError("Deferred renderer initialization failed: scene texture creation failed");
        return false;
    }

    if (!CreateDescriptorHeap(Device))
    {
        LogError("Deferred renderer initialization failed: descriptor heap creation failed");
        return false;
    }

    if (Gtao && !Gtao->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: GTAO descriptor creation failed");
        return false;
    }

    if (PathTracing && !PathTracing->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: path tracing descriptor creation failed");
        return false;
    }

    if (RestirGI && !RestirGI->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI descriptor creation failed");
        return false;
    }

    if (RestirGIDenoiser && !RestirGIDenoiser->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI denoiser descriptor creation failed");
        return false;
    }

    if (AutoExposure && !AutoExposure->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: auto exposure descriptor creation failed");
        return false;
    }

    if (Cas && !Cas->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: CAS descriptor creation failed");
        return false;
    }

    if (Taa && !Taa->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: TAA descriptor creation failed");
        return false;
    }

    if (Tonemap && !Tonemap->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: tonemap descriptor creation failed");
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeSkyResources(FDX12Device* Device)
{
    SkySphereRadius = (std::max)(SceneRadius * 5.0f, 100.0f);
    if (!RendererUtils::CreateSkyAtmosphereResources(Device, SkySphereRadius, SkyGeometry, SkyConstantBuffer, SkyConstantBufferMapped))
    {
        LogError("Deferred renderer initialization failed: sky resource creation failed");
        return false;
    }

    if (SkyConstantBuffer)
    {
        SkyConstantBuffer->SetName(L"SkyConstantBuffer");
    }

    FSkyPipelineConfig SkyPipelineConfig;
    SkyPipelineConfig.DepthEnable = true;
    SkyPipelineConfig.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    SkyPipelineConfig.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    SkyPipelineConfig.DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    if (!RendererUtils::CreateSkyAtmospherePipeline(Device, LightingBufferFormat, SkyPipelineConfig, SkyRootSignature, SkyPipelineState))
    {
        LogError("Deferred renderer initialization failed: sky pipeline state creation failed");
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeGpuDebugResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    if (!bEnableGpuDebugPrint)
    {
        return true;
    }

    if (!CreateGpuDebugPrintResources(Device) || !CreateGpuDebugPrintPipeline(Device, BackBufferFormat) || !CreateGpuDebugLinePipeline(Device, BackBufferFormat) || !CreateGpuDebugPrintStatsPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: GPU debug print setup failed");
        return false;
    }

    return true;
}

void FDeferredRenderer::RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime)
{
    if (HasRenderFatalError())
    {
        const float FatalClearColor[4] = { 0.05f, 0.0f, 0.1f, 1.0f };
        CmdContext.ClearRenderTarget(RtvHandle, FatalClearColor);
        return;
    }

    const bool bAnySkinningUpdated = RendererUtils::UpdateGltfSceneAnimation(SceneModels, GltfScenes, GltfScenePoses, GltfSceneTimes, DeltaTime);

    PrepareGpuDebugPrint(CmdContext);

    FDeferredFrameState FrameState;
    PrepareFrameState(Camera, bAnySkinningUpdated, FrameState);
    DispatchSkinning(CmdContext, FrameState.LightViewProjection);
    UpdateRayTracingBlasRefit(CmdContext);
    BuildRayTracingTlas(CmdContext);

    FRenderGraph Graph;
    ConfigureFrameGraph(Graph);

    const bool bUsePathTracing = bPathTracingEnabled && bRayTracingPipelineReady;

    FDeferredFrameResources Resources;
    FDeferredPassContext PassContext
    {
        *this,
        Graph,
        FrameState,
        Resources,
        Camera,
        GetFrameIndex() % GetFramesInFlight(),
        DeltaTime,
        bUsePathTracing,
        RtvHandle
    };
    ResourceImporter->ImportFrameResources(PassContext);

    ConfigureHZBOcclusion(FrameState.bUseHZBOcclusion, HZBSrvBindlessIndex, HZBWidth, HZBHeight, HZBMipCount);

    FrameOrchestrator->BuildFrameGraph(PassContext);

    Graph.Execute(CmdContext);

    FinalizeFrameState(FrameState);
}

void FDeferredRenderer::PrepareFrameState(const FCamera& Camera, bool bAnySkinningUpdated, FDeferredFrameState& OutState)
{
    UpdateCullingVisibility(Camera);

    bool bCameraMoved = false;
    if (!bFirstFrame)
    {
        const FFloat3 CurrentPosition = Camera.GetPosition();
        const DirectX::XMFLOAT3 CurrentPosXM(CurrentPosition.x, CurrentPosition.y, CurrentPosition.z);
        
        DirectX::XMFLOAT4X4 CurrentViewMatrix;
        DirectX::XMStoreFloat4x4(&CurrentViewMatrix, Camera.GetViewMatrix());
        
        const float PositionThreshold = 0.001f;
        const bool bPositionChanged = 
            std::abs(CurrentPosXM.x - PreviousCameraPosition.x) > PositionThreshold ||
            std::abs(CurrentPosXM.y - PreviousCameraPosition.y) > PositionThreshold ||
            std::abs(CurrentPosXM.z - PreviousCameraPosition.z) > PositionThreshold;
        
        bool bViewMatrixChanged = false;
        for (int i = 0; i < 16; ++i)
        {
            if (std::abs(reinterpret_cast<const float*>(&CurrentViewMatrix)[i] - 
                         reinterpret_cast<const float*>(&PreviousCameraViewMatrix)[i]) > 0.0001f)
            {
                bViewMatrixChanged = true;
                break;
            }
        }
        
        bCameraMoved = bPositionChanged || bViewMatrixChanged;
        
        PreviousCameraPosition = CurrentPosXM;
        PreviousCameraViewMatrix = CurrentViewMatrix;
    }
    else
    {
        const FFloat3 CurrentPosition = Camera.GetPosition();
        PreviousCameraPosition = DirectX::XMFLOAT3(CurrentPosition.x, CurrentPosition.y, CurrentPosition.z);
        DirectX::XMStoreFloat4x4(&PreviousCameraViewMatrix, Camera.GetViewMatrix());
        bFirstFrame = false;
    }

    OutState.bCameraMoved = bCameraMoved;
    OutState.bAnySkinningUpdated = bAnySkinningUpdated;

    if (!bHasPreviousViewProjection)
    {
        if (RestirGI)
        {
            RestirGI->InvalidateReservoirHistory();
        }
        InvalidateRestirGiDenoiserHistory();
    }

    if (!IsRestirGIEnabled())
    {
        if (RestirGI)
        {
            RestirGI->InvalidateReservoirHistory();
        }
        InvalidateRestirGiDenoiserHistory();
    }

    if (!IsRestirGIDenoiserEnabled())
    {
        InvalidateRestirGiDenoiserHistory();
    }

    OutState.bGtaoJitterActive = bGtaoEnabled && bGtaoJitterEnabled;
    if (Taa)
    {
        Taa->PrepareFrameState(
            *this,
            Camera,
            OutState.bGtaoJitterActive,
            OutState.bTaaActive,
            OutState.bTaaHistoryReady,
            OutState.TaaFrameIndex,
            OutState.TaaReadIndex,
            OutState.TaaWriteIndex);
    }

    if (PathTracing)
    {
        PathTracing->PrepareFrameState(
            OutState.TaaFrameIndex,
            bCameraMoved,
            OutState.bPathTracingAccumulationActive,
            OutState.bPathTracingAccumulationHistoryReady,
            OutState.PathTracingAccumulationReadIndex,
            OutState.PathTracingAccumulationWriteIndex);
    }

    const bool bUseTaaJitter = Taa && Taa->UsesJitter();
    const DirectX::XMMATRIX CurrentProjection = bUseTaaJitter ? Taa->GetProjection() : Camera.GetProjectionMatrix();
    const DirectX::XMMATRIX CurrentViewProjection = Camera.GetViewMatrix() * CurrentProjection;
    DirectX::XMStoreFloat4x4(&CurrentViewProjectionMatrix, CurrentViewProjection);

    const DirectX::XMMATRIX CurrentUnjitteredProjection = Camera.GetProjectionMatrix();
    const DirectX::XMMATRIX CurrentUnjitteredViewProjection = Camera.GetViewMatrix() * CurrentUnjitteredProjection;
    DirectX::XMStoreFloat4x4(&CurrentUnjitteredViewProjectionMatrix, CurrentUnjitteredViewProjection);

    OutState.bRenderShadows = bShadowsEnabled && ShadowPipelines[0] && ShadowPipelines[1] && ShadowMap;
    OutState.bDoDepthPrepass = bDepthPrepassEnabled && DepthPrepassPipelines[0] && DepthPrepassPipelines[1];
    if (!bHZBEnabled)
    {
        bHZBReady = false;
    }

    const bool bPrevHZBReady = bHZBReady;
    OutState.bUseHZBOcclusion = bHZBEnabled && bPrevHZBReady && HZBSrvBindlessIndex != UINT32_MAX;
    OutState.bUseHzbTwoPass = bEnableIndirectDraw && OutState.bUseHZBOcclusion && bEnableHzbTwoPass;
    if (OutState.bUseHzbTwoPass)
    {
        OutState.bDoDepthPrepass = false;
    }
    OutState.bBuildHZB = bHZBEnabled;
    OutState.bCasActive = Cas && Cas->IsReady();
    OutState.LightViewProjection = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
}

void FDeferredRenderer::ConfigureFrameGraph(FRenderGraph& Graph) const
{
    Graph.SetDevice(Device);
    Graph.SetBarrierLoggingEnabled(bLogResourceBarriers);
    Graph.SetGraphDumpEnabled(bEnableGraphDump);
    Graph.SetGpuTimingEnabled(bEnableGpuTiming);
}

void FDeferredRenderer::FinalizeFrameState(const FDeferredFrameState& FrameState)
{
    if (Taa)
    {
        Taa->FinalizeFrameState(FrameState.bTaaActive, FrameState.bGtaoJitterActive);
    }

    if (AutoExposure)
    {
        AutoExposure->FinalizeFrame();
    }

    PreviousViewProjectionMatrix = CurrentViewProjectionMatrix;
    bHasPreviousViewProjection = true;
    PreviousUnjitteredViewProjectionMatrix = CurrentUnjitteredViewProjectionMatrix;
    bHasPreviousUnjitteredViewProjection = true;

    if (RestirGI)
    {
        RestirGI->FinalizeFrame(*this);
    }

    if (RestirGIDenoiser)
    {
        RestirGIDenoiser->FinalizeFrame(*this);
    }

    for (FSceneModelResource& Model : SceneModels)
    {
        Model.PreviousWorldMatrix = Model.WorldMatrix;
        Model.bHasPreviousWorldMatrix = true;
    }
}

void FDeferredRenderer::InvalidateRestirGiDenoiserHistory()
{
    if (RestirGIDenoiser)
    {
        RestirGIDenoiser->InvalidateHistory();
    }
}

void FDeferredRenderer::OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue)
{
    (void)FenceValue;

    if (Taa)
    {
        Taa->OnFrameFenceSignaled(FrameIndex);
    }

    if (PathTracing)
    {
        PathTracing->OnFrameFenceSignaled(FrameIndex);
    }
}


void FDeferredRenderer::UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, size_t ModelIndex, uint64_t ConstantBufferOffset)
{
    (void)ModelIndex;

    const DirectX::XMVECTOR LightDir = DirectX::XMLoadFloat3(&LightDirection);
    const DirectX::XMMATRIX LightVP = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
    DirectX::XMStoreFloat4x4(&LightViewProjection, LightVP);
    const bool bUseTaaJitter = Taa && Taa->UsesJitter();
    const DirectX::XMMATRIX Projection = bUseTaaJitter ? Taa->GetProjection() : Camera.GetProjectionMatrix();
    const DirectX::XMFLOAT2 Jitter = bUseTaaJitter ? Taa->GetJitter() : DirectX::XMFLOAT2(0.0f, 0.0f);
    const uint32_t TaaSampleIndex = Taa ? Taa->GetSampleIndex() : 0u;
    const uint32_t GtaoTemporalIndex = (bGtaoEnabled && bGtaoJitterEnabled) ? TaaSampleIndex : 0u;

    const DirectX::XMMATRIX PreviousWorld = Model.bHasPreviousWorldMatrix
        ? DirectX::XMLoadFloat4x4(&Model.PreviousWorldMatrix)
        : DirectX::XMMatrixIdentity();
    const bool bHasPreviousWorld = Model.bHasPreviousWorldMatrix;

    uint32_t PreviousSkinnedPositionBindlessIndex = UINT32_MAX;
    bool bHasPreviousSkinning = false;
    const uint32_t FrameCount = GetFramesInFlight();
    const uint32_t PrevFrameIndex = FrameCount > 0 ? (GetFrameIndex() + FrameCount - 1u) % FrameCount : 0u;
    if (Model.BoneMatrixBindlessIndex != UINT32_MAX
        && Model.BoneMatrixCount > 0
        && PrevFrameIndex < Model.SkinnedPositionSrvBindlessIndices.size())
    {
        PreviousSkinnedPositionBindlessIndex = Model.SkinnedPositionSrvBindlessIndices[PrevFrameIndex];
        bHasPreviousSkinning = PreviousSkinnedPositionBindlessIndex != UINT32_MAX;
    }

    RendererUtils::UpdateSceneConstants(
        Camera,
        Model,
        LightIntensity,
        LightDir,
        LightColor,
        LightVP,
        Projection,
        bShadowsEnabled ? ShadowStrength : 0.0f,
        ShadowBias,
        static_cast<float>(ShadowMapWidth),
        static_cast<float>(ShadowMapHeight),
        EnvironmentMipCount,
        Jitter,
        GtaoTemporalIndex,
        bGtaoEnabled,
        GtaoRadius,
        GtaoIntensity,
        GtaoPower,
        GtaoThickness,
        GtaoDirectionCount,
        GtaoStepCount,
        GetSceneConstantBufferMapped(),
        ConstantBufferOffset,
        DirectX::XMLoadFloat4x4(&PreviousViewProjectionMatrix),
        bHasPreviousViewProjection,
        PreviousWorld,
        bHasPreviousWorld,
        PreviousSkinnedPositionBindlessIndex,
        bHasPreviousSkinning);
}

void FDeferredRenderer::UpdateSkyConstants(const FCamera& Camera)
{
    using namespace DirectX;

    const FFloat3 CameraPosition = Camera.GetPosition();
    const XMMATRIX Scale = XMMatrixScaling(SkySphereRadius, SkySphereRadius, SkySphereRadius);
    const XMMATRIX Translation = XMMatrixTranslation(CameraPosition.x, CameraPosition.y, CameraPosition.z);
    const XMMATRIX World = Scale * Translation;

    const XMVECTOR LightDir = XMLoadFloat3(&LightDirection);
    const bool bUseTaaJitter = Taa && Taa->UsesJitter();
    const DirectX::XMMATRIX Projection = bUseTaaJitter ? Taa->GetProjection() : Camera.GetProjectionMatrix();
    RendererUtils::UpdateSkyConstants(Camera, World, Projection, LightDir, LightColor, SkyConstantBufferMapped);
}

void FDeferredRenderer::UpdateCullingVisibility(const FCamera& Camera)
{
    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    const bool bGpuCullingActive = bEnableIndirectDraw && CullingPipeline && CullingRootSignature && GetIndirectCommandBuffer()
        && ModelBoundsBuffer && MeshletConeAxisBuffer && MeshletConeApexBuffer;
    RendererUtils::UpdateCullingVisibility(*CullingCamera, SceneModels, SceneModelVisibility, !bGpuCullingActive);
}
