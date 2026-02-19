#ifndef PATH_BRDF_COMMON_HLSLI
#define PATH_BRDF_COMMON_HLSLI

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

float3 BRDF(float3 wi, float3 wo, float3 N, float3 diffuse, float3 specular, float roughness)
{
    float3 F;
    float3 specularBrdf = SpecularBRDF(N, wo, wi, specular, roughness, F);
    float3 diffuseBrdf = DiffuseBRDF(diffuse);
    return diffuseBrdf + specularBrdf;
}

#endif
