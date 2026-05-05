#ifndef PATH_TRACING_COMMON_HLSL
#define PATH_TRACING_COMMON_HLSL

#include "../Common.hlsli"

float3 HashToColor(uint value)
{
    uint h = Hash32(value);
    float r = ((h >> 0) & 0xff) / 255.0f;
    float g = ((h >> 8) & 0xff) / 255.0f;
    float b = ((h >> 16) & 0xff) / 255.0f;
    return float3(r, g, b);
}

// PDF = cos / PI = NdotL / PI
float3 SampleCosHemisphere(float2 randVal, float3 N)
{
    float r = sqrt(randVal.x);
    float phi = 2.0 * PI * randVal.y;

    float sinPhi, cosPhi;
    sincos(phi, sinPhi, cosPhi);

    float3 v = float3(r * cos(phi), r * sin(phi), sqrt(1.0 - randVal.x));

    return TangentToWorld(v, N);
}

float3 SampleConeUniform(float2 randVal, float radius, float3 direction)
{
    float cosTheta = cos(radius);
    float r0 = cosTheta + randVal.x * (1.0f - cosTheta);
    float r = sqrt(max(0.0f, 1.0f - r0 * r0));
    float phi = 2.0f * PI * randVal.y;

    float sinPhi;
    float cosPhi;
    sincos(phi, sinPhi, cosPhi);

    float3 v = float3(r * cosPhi, r * sinPhi, r0);
    return TangentToWorld(v, direction);
}

float Luminance(float3 color)
{
    return dot(max(color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}

float G1_SmithGGX(float NdotV, float alpha)
{
    float a2 = alpha * alpha;
    float NdotV2 = NdotV * NdotV;
    float tan2 = max((1.0f - NdotV2) / max(NdotV2, 1e-6f), 0.0f);
    return 2.0f / (1.0f + sqrt(1.0f + a2 * tan2));
}

float3 SampleGGX_VNDF(float3 Vt, float alpha, float2 randVal)
{
    float3 Vh = normalize(float3(alpha * Vt.x, alpha * Vt.y, Vt.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = (lensq > 0.0f) ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq) : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    float r = sqrt(randVal.x);
    float phi = 2.0f * PI * randVal.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5f * (1.0f + Vh.z);
    t2 = lerp(sqrt(max(0.0f, 1.0f - t1 * t1)), t2, s);

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;
    float3 Ht = normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0f, Nh.z)));
    return Ht;
}

// GGX VNDF reflection PDF (matches Heitz VNDF + reflection Jacobian)
// p(H) = D(H) * G1(V) * (V·H) / (N·V)
// p(wi) = p(H) / (4*(V·H)) = D(H) * G1(V) / (4*(N·V))
float Pdf_GGX_VNDF_Reflection(float3 N, float3 V, float3 H, float alpha)
{
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));

    float D = D_GGX(NdotH, alpha);
    float G1 = G1_SmithGGX(NdotV, alpha);

    return (D * G1) / max(4.0f * NdotV, 1e-6f);
}

// Heitz VNDF (JCGT 2018) closed-form PDF for *reflected direction* sampling
// samplePDF = D / (2 * (NdotV + sqrt(NdotV*(NdotV - NdotV*a2) + a2)))
float Pdf_GGX_VNDF_ClosedForm(float3 N, float3 V, float3 H, float alpha)
{
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float D = D_GGX(NdotH, alpha);

    float a2 = alpha * alpha;
    float denom = 2.0f * (NdotV + sqrt(max(0.0f, a2 + (1.0f - a2) * (NdotV * NdotV))));
    return D / max(denom, 1e-6f);
}

#endif
