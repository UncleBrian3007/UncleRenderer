// 공통 PBR 계산을 위한 유틸리티 함수 모음
static const float PI = 3.14159265f;

// GGX/Trowbridge-Reitz 분포: D = a^2 / (pi * ((N·H)^2 * (a^2 - 1) + 1)^2)
float DistributionGGX(float NdotH, float alpha)
{
    float alpha2 = alpha * alpha;
    float denom = (NdotH * NdotH) * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / max(PI * denom * denom, 1e-4f);
}

// Smith Schlick-GGX 근사: Gx = N·X / ((N·X)(1-k)+k)
float GeometrySchlickGGX(float NdotX, float k)
{
    return NdotX / (NdotX * (1.0f - k) + k);
}

// Schlick 근사 프레넬: F = F0 + (1 - F0)(1 - V·H)^5
float3 FresnelSchlick(float VdotH, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
}

// PBR 스펙/디퓨즈 합산 계산을 공통화한 함수
float3 EvaluatePBR(float3 albedo, float metallic, float roughness, float3 F0, float3 N, float3 V, float3 L)
{
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float alpha = roughness * roughness;
    float D = DistributionGGX(NdotH, alpha);

    float k = (roughness + 1.0f);
    k = (k * k) / 8.0f;
    float G = GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);

    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotL * NdotV, 1e-4f);
    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kd * albedo;// / PI;

    return (diffuse + specular) * NdotL;
}
