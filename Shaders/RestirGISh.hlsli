#ifndef RESTIR_GI_SH_HLSLI
#define RESTIR_GI_SH_HLSLI

#include "OctahedralEncoding.hlsli"

// 1st-order SH basis constants.
static const float SH_BASIS_L0 = 0.28209479177387814f;
static const float SH_BASIS_L1 = 0.48860251190291992f;
static const float SH_PI = 3.14159265358979324f;
typedef float4 sh2;

struct SH
{
    // 1st-order SH coefficients for luminance (Y in YCoCg).
    sh2 ShY;
    // Chrominance red-blue difference term in YCoCg space.
    float Co;
    // Chrominance green-luma difference term in YCoCg space.
    float Cg;
};

typedef SH FRestirGiPackedSh;

float3 RestirGiRgbToYCoCg(float3 Rgb)
{
    const float Y = dot(Rgb, float3(0.25f, 0.5f, 0.25f));
    const float Co = Rgb.r - Rgb.b;
    const float Cg = Rgb.g - Y;
    return float3(Y, Co, Cg);
}

uint RestirGiEncodeDirection16x2(float3 Direction)
{
    return EncodeOctahedral16x2(Direction);
}

float3 RestirGiDecodeDirection16x2(uint Packed)
{
    return DecodeOctahedral16x2(Packed);
}

float3 RestirGiYCoCgToRgb(float3 YCoCg)
{
    const float Y = YCoCg.x;
    const float Co = YCoCg.y;
    const float Cg = YCoCg.z;
    return float3(
        Y + Co * 0.5f - Cg,
        Y + Cg,
        Y - Co * 0.5f - Cg);
}

sh2 RestirGiShEvaluate(float3 Direction)
{
    const float3 D = normalize(Direction);
    return sh2(
        SH_BASIS_L0,
        -SH_BASIS_L1 * D.y,
        SH_BASIS_L1 * D.z,
        -SH_BASIS_L1 * D.x);
}

sh2 RestirGiShScale(sh2 Sh, float Scale)
{
    return Sh * Scale;
}

sh2 RestirGiShAdd(sh2 A, sh2 B)
{
    return A + B;
}

sh2 RestirGiApplyDiffuseConvolutionL1(sh2 RadianceSh)
{
    const float A0 = 0.886227f;
    const float A1 = 1.023326f;
    sh2 Result;
    Result.x = RadianceSh.x * A0;
    Result.yzw = RadianceSh.yzw * A1;
    return Result;
}

float RestirGiShUnproject(sh2 FunctionSh, float3 Direction)
{
    const sh2 Basis = RestirGiShEvaluate(Direction);
    return dot(FunctionSh, Basis);
}

FRestirGiPackedSh RestirGiProjectSh(float3 Radiance, float3 Direction)
{
    FRestirGiPackedSh Sh;
    const float3 YCoCg = RestirGiRgbToYCoCg(max(Radiance, 0.0f.xxx));
    Sh.ShY = RestirGiShScale(RestirGiShEvaluate(Direction), 2.0f * SH_PI * YCoCg.x);
    Sh.Co = YCoCg.y;
    Sh.Cg = YCoCg.z;
    return Sh;
}

FRestirGiPackedSh RestirGiAddSh(FRestirGiPackedSh A, FRestirGiPackedSh B)
{
    FRestirGiPackedSh Out;
    Out.ShY = RestirGiShAdd(A.ShY, B.ShY);
    Out.Co = A.Co + B.Co;
    Out.Cg = A.Cg + B.Cg;
    return Out;
}

FRestirGiPackedSh RestirGiScaleSh(FRestirGiPackedSh A, float Scale)
{
    FRestirGiPackedSh Out;
    Out.ShY = RestirGiShScale(A.ShY, Scale);
    Out.Co = A.Co * Scale;
    Out.Cg = A.Cg * Scale;
    return Out;
}

FRestirGiPackedSh RestirGiLerpSh(FRestirGiPackedSh A, FRestirGiPackedSh B, float T)
{
    return RestirGiAddSh(RestirGiScaleSh(A, 1.0f - T), RestirGiScaleSh(B, T));
}

float3 RestirGiApproxRadiance(FRestirGiPackedSh Sh)
{
    const float Y = max(0.0f, Sh.ShY.x / max(2.0f * SH_PI * SH_BASIS_L0, 1e-6f));
    return max(RestirGiYCoCgToRgb(float3(Y, Sh.Co, Sh.Cg)), 0.0f.xxx);
}

uint4 RestirGiPackSh(FRestirGiPackedSh Sh)
{
    uint4 Packed;
    Packed.x = (f32tof16(Sh.ShY.x) << 16) | f32tof16(Sh.ShY.y);
    Packed.y = (f32tof16(Sh.ShY.z) << 16) | f32tof16(Sh.ShY.w);
    Packed.z = asuint(Sh.Co);
    Packed.w = asuint(Sh.Cg);
    return Packed;
}

FRestirGiPackedSh RestirGiUnpackSh(uint4 Packed)
{
    FRestirGiPackedSh Sh;
    Sh.ShY.x = f16tof32(Packed.x >> 16);
    Sh.ShY.y = f16tof32(Packed.x & 0xFFFFu);
    Sh.ShY.z = f16tof32(Packed.y >> 16);
    Sh.ShY.w = f16tof32(Packed.y & 0xFFFFu);
    Sh.Co = asfloat(Packed.z);
    Sh.Cg = asfloat(Packed.w);
    return Sh;
}

float3 RestirGiUnprojectIrradiance(FRestirGiPackedSh Sh, float3 SurfaceNormal)
{
    const sh2 IrradianceSh = RestirGiApplyDiffuseConvolutionL1(Sh.ShY);
    const float Y = max(0.0f, RestirGiShUnproject(IrradianceSh, normalize(SurfaceNormal)));
    return max(RestirGiYCoCgToRgb(float3(Y, Sh.Co, Sh.Cg)), 0.0f.xxx);
}

float RestirGiShVariance(FRestirGiPackedSh Sh)
{
    const float Y = max(0.0f, Sh.ShY.x / max(2.0f * SH_PI * SH_BASIS_L0, 1e-6f));
    const float Directional = saturate(length(Sh.ShY.yzw) / max(abs(Sh.ShY.x), 1e-5f));
    return saturate(Y * 0.1f + Directional * 0.25f);
}

#endif
