#ifndef COMMON_SH_HLSLI
#define COMMON_SH_HLSLI

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

typedef SH FPackedSh;

float3 RgbToYCoCg(float3 Rgb)
{
    const float Y = dot(Rgb, float3(0.25f, 0.5f, 0.25f));
    const float Co = Rgb.r - Rgb.b;
    const float Cg = Rgb.g - Y;
    return float3(Y, Co, Cg);
}

uint EncodeDirection16x2(float3 Direction)
{
    return EncodeOctahedral16x2(Direction);
}

float3 DecodeDirection16x2(uint Packed)
{
    return DecodeOctahedral16x2(Packed);
}

float3 YCoCgToRgb(float3 YCoCg)
{
    const float Y = YCoCg.x;
    const float Co = YCoCg.y;
    const float Cg = YCoCg.z;
    return float3(
        Y + Co * 0.5f - Cg,
        Y + Cg,
        Y - Co * 0.5f - Cg);
}

sh2 ShEvaluate(float3 Direction)
{
    const float3 D = normalize(Direction);
    return sh2(
        SH_BASIS_L0,
        -SH_BASIS_L1 * D.y,
        SH_BASIS_L1 * D.z,
        -SH_BASIS_L1 * D.x);
}

sh2 ShScale(sh2 Sh, float Scale)
{
    return Sh * Scale;
}

sh2 ShAdd(sh2 A, sh2 B)
{
    return A + B;
}

sh2 ApplyDiffuseConvolutionL1(sh2 RadianceSh)
{
    const float A0 = 0.886227f;
    const float A1 = 1.023326f;
    sh2 Result;
    Result.x = RadianceSh.x * A0;
    Result.yzw = RadianceSh.yzw * A1;
    return Result;
}

float ShUnproject(sh2 FunctionSh, float3 Direction)
{
    const sh2 Basis = ShEvaluate(Direction);
    return dot(FunctionSh, Basis);
}

FPackedSh ProjectSh(float3 Radiance, float3 Direction)
{
    FPackedSh Sh;
    const float3 YCoCg = RgbToYCoCg(max(Radiance, 0.0f.xxx));
    Sh.ShY = ShScale(ShEvaluate(Direction), 2.0f * SH_PI * YCoCg.x);
    Sh.Co = YCoCg.y;
    Sh.Cg = YCoCg.z;
    return Sh;
}

// Projects an already-integrated, directionless irradiance into a DC-only SH so that
// UnprojectIrradiance(Sh, anyNormal) returns the same irradiance (isotropic round-trip).
// The DC scale is the inverse of the L0 diffuse-convolution gain (0.886227 * SH_BASIS_L0).
FPackedSh ProjectIrradianceSh(float3 Irradiance)
{
    FPackedSh Sh;
    const float3 YCoCg = RgbToYCoCg(max(Irradiance, 0.0f.xxx));
    Sh.ShY = float4(YCoCg.x / (0.886227f * SH_BASIS_L0), 0.0f, 0.0f, 0.0f);
    Sh.Co = YCoCg.y;
    Sh.Cg = YCoCg.z;
    return Sh;
}

FPackedSh AddSh(FPackedSh A, FPackedSh B)
{
    FPackedSh Out;
    Out.ShY = ShAdd(A.ShY, B.ShY);
    Out.Co = A.Co + B.Co;
    Out.Cg = A.Cg + B.Cg;
    return Out;
}

FPackedSh ScaleSh(FPackedSh A, float Scale)
{
    FPackedSh Out;
    Out.ShY = ShScale(A.ShY, Scale);
    Out.Co = A.Co * Scale;
    Out.Cg = A.Cg * Scale;
    return Out;
}

FPackedSh LerpSh(FPackedSh A, FPackedSh B, float T)
{
    return AddSh(ScaleSh(A, 1.0f - T), ScaleSh(B, T));
}

float3 ApproxRadiance(FPackedSh Sh)
{
    const float Y = max(0.0f, Sh.ShY.x / max(2.0f * SH_PI * SH_BASIS_L0, 1e-6f));
    return max(YCoCgToRgb(float3(Y, Sh.Co, Sh.Cg)), 0.0f.xxx);
}

uint4 PackSh(FPackedSh Sh)
{
    uint4 Packed;
    Packed.x = (f32tof16(Sh.ShY.x) << 16) | f32tof16(Sh.ShY.y);
    Packed.y = (f32tof16(Sh.ShY.z) << 16) | f32tof16(Sh.ShY.w);
    Packed.z = asuint(Sh.Co);
    Packed.w = asuint(Sh.Cg);
    return Packed;
}

FPackedSh UnpackSh(uint4 Packed)
{
    FPackedSh Sh;
    Sh.ShY.x = f16tof32(Packed.x >> 16);
    Sh.ShY.y = f16tof32(Packed.x & 0xFFFFu);
    Sh.ShY.z = f16tof32(Packed.y >> 16);
    Sh.ShY.w = f16tof32(Packed.y & 0xFFFFu);
    Sh.Co = asfloat(Packed.z);
    Sh.Cg = asfloat(Packed.w);
    return Sh;
}

float3 UnprojectIrradiance(FPackedSh Sh, float3 SurfaceNormal)
{
    const sh2 IrradianceSh = ApplyDiffuseConvolutionL1(Sh.ShY);
    const float Y = max(0.0f, ShUnproject(IrradianceSh, normalize(SurfaceNormal)));
    return max(YCoCgToRgb(float3(Y, Sh.Co, Sh.Cg)), 0.0f.xxx);
}

float ShVariance(FPackedSh Sh)
{
    const float Y = max(0.0f, Sh.ShY.x / max(2.0f * SH_PI * SH_BASIS_L0, 1e-6f));
    const float Directional = saturate(length(Sh.ShY.yzw) / max(abs(Sh.ShY.x), 1e-5f));
    return saturate(Y * 0.1f + Directional * 0.25f);
}

#endif
