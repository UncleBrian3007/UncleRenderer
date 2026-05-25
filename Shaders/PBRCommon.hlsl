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
float3 EvaluatePBR_Research(float3 albedo, float metallic, float roughness, float3 F0, float3 N, float3 V, float3 L)
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
    float3 diffuse = kd * albedo / PI;

    return (diffuse + specular) * NdotL;
}

float D_GGX(float NdotH, float a)
{
	float a2 = a * a;

	float denom = (NdotH * NdotH * (a2 - 1.0f) + 1.0f);
	denom = PI * denom * denom;

	return a2 * rcp(denom);
}

//http://graphicrants.blogspot.com/2013/08/specular-brdf-reference.html
// Smith Joint GGX
float V_SmithGGX(float NdotV, float NdotL, float a)
{
	float a2 = a * a;

	float G_V = NdotV + sqrt((NdotV - NdotV * a2) * NdotV + a2);
	float G_L = NdotL + sqrt((NdotL - NdotL * a2) * NdotL + a2);
	return rcp(G_V * G_L);
}

float3 DiffuseBRDF(float3 diffuse)
{
    return diffuse / PI;
}

float3 SpecularBRDF(float3 N, float3 V, float3 L, float3 specular, float roughness, out float3 F)
{
    float3 H = normalize(V + L);

    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float alpha = roughness * roughness;
    float D = D_GGX(NdotH, alpha);
    float V_G = V_SmithGGX(NdotV, NdotL, alpha);
    F = FresnelSchlick(VdotH, specular);

    return D * V_G * F;
}

// PBR 스펙/디퓨즈 합산 계산을 공통화한 함수
float3 EvaluatePBR(float3 albedo, float metallic, float roughness, float3 F0, float3 N, float3 V, float3 L)
{
	float NdotL = saturate(dot(N, L));

	roughness = max(roughness, 0.03f);

	float3 F;
	float3 specular = SpecularBRDF(N, V, L, F0, roughness, F);
	float3 kd = (1.0f - F) * (1.0f - metallic);
	float3 diffuse = kd * albedo / PI;

	return (diffuse + specular) * NdotL;
}

float3 EvaluateSheenLobe(float3 sheenColor, float sheenRoughness, float3 N, float3 V, float3 L)
{
    float NdotL = saturate(dot(N, L));
    float roughness = max(sheenRoughness, 0.03f);
    float3 F;
    float3 specular = SpecularBRDF(N, V, L, sheenColor, roughness, F);
    return specular * NdotL;
}

float3 EvaluatePBRWithSheen(float3 albedo, float metallic, float roughness, float3 F0, float3 N, float3 V, float3 L, float3 sheenColor, float sheenRoughness)
{
    float3 baseLighting = EvaluatePBR(albedo, metallic, roughness, F0, N, V, L);
    float3 sheenLighting = EvaluateSheenLobe(sheenColor, sheenRoughness, N, V, L);
    return baseLighting + sheenLighting;
}

float3 EvaluateClearcoatLobe(float clearcoat, float clearcoatRoughness, float3 N, float3 V, float3 L)
{
    float NdotL = saturate(dot(N, L));
    float roughness = max(clearcoatRoughness, 0.03f);
    float3 F;
    float3 specular = SpecularBRDF(N, V, L, 0.04.xxx, roughness, F);
    return specular * NdotL * clearcoat;
}

float3 EvaluatePBRWithClearcoat(float3 albedo, float metallic, float roughness, float3 F0, float3 N, float3 V, float3 L, float clearcoat, float clearcoatRoughness)
{
    float3 baseLighting = EvaluatePBR(albedo, metallic, roughness, F0, N, V, L);
    float3 clearcoatLighting = EvaluateClearcoatLobe(clearcoat, clearcoatRoughness, N, V, L);
    return baseLighting + clearcoatLighting;
}

float3 EvaluatePBRWithAnisotropy(float3 albedo, float metallic, float roughness, float3 F0, float3 N, float3 V, float3 L, float anisotropyValue, float anisotropyStrength)
{
    float anisotropy = saturate(anisotropyValue * anisotropyStrength);
    float anisotropicRoughness = lerp(roughness, max(0.03f, roughness * 0.5f), anisotropy);
    return EvaluatePBR(albedo, metallic, anisotropicRoughness, F0, N, V, L);
}

float3 BRDF(float3 wi, float3 wo, float3 N, float3 diffuse, float3 specular, float roughness)
{
    float3 F;
    float3 specularBrdf = SpecularBRDF(N, wo, wi, specular, roughness, F);
    float3 diffuseBrdf = DiffuseBRDF(diffuse) * (1.0f - F);
    return diffuseBrdf + specularBrdf;
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
