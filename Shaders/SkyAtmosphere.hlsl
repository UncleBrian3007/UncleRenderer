struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float4 Tangent  : TANGENT;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 WorldPos : TEXCOORD0;
    float3 Normal   : TEXCOORD1;
};

cbuffer SkyConstants : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 View;
    row_major float4x4 Projection;
    float3 CameraPosition;
    float Padding0;
    float3 LightDirection;
    float Padding1;
    float3 LightColor;
    float Padding2;
};

VSOutput VSMain(VSInput Input)
{
    VSOutput Output;
    float4 WorldPosition = mul(float4(Input.Position, 1.0), World);
    float4 ViewPosition = mul(WorldPosition, View);
    Output.Position = mul(ViewPosition, Projection);
    Output.WorldPos = WorldPosition.xyz;
    Output.Normal = mul(Input.Normal, (float3x3)World);
    return Output;
}

float RayleighPhase(float cosTheta)
{
    // Analytical Rayleigh scattering phase function: (3 / 16π) * (1 + cos²θ).
    // Derived from the dipole scattering model assuming unpolarized light and
    // small particles relative to the wavelength, resulting in the characteristic
    // blue hue (short wavelengths scattered more strongly).
    const float k = 3.0 / (16.0 * 3.14159265);
    return k * (1.0 + cosTheta * cosTheta);
}

float MiePhase(float cosTheta, float g)
{
    // Cornette–Shanks approximation of the Henyey–Greenstein phase function:
    // (1 - g²) / (4π * (1 + g² - 2g cosθ)^(3/2)). The asymmetry factor g controls
    // how strongly the scattering is pushed forward (g → 1) or backward (g → -1).
    float g2 = g * g;
    float denom = pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5);
    return (1.0 - g2) / (4.0 * 3.14159265 * max(denom, 1e-3));
}

float3 ApplyAtmosphere(float3 viewDir)
{
    const float horizonFalloff = saturate(pow(1.0 - saturate(viewDir.y * 0.5 + 0.5), 3.0));
    const float3 zenithColor = float3(0.05, 0.12, 0.22);
    const float3 horizonColor = float3(0.52, 0.68, 0.86);
    float3 baseSky = lerp(zenithColor, horizonColor, horizonFalloff);

    float3 up = float3(0.0, 1.0, 0.0);
    float cosSunView = dot(viewDir, normalize(LightDirection));
    float cosSunUp = dot(normalize(LightDirection), up);

    // Simple exponential atmosphere: density = exp(-height / H) for both
    // Rayleigh and Mie components, where H is the scale height controlling the
    // rate at which particle concentration drops with altitude.
    const float rayleighScaleHeight = 8000.0;
    const float mieScaleHeight = 1200.0;
    float viewHeight = max(0.0, CameraPosition.y);
    float rayleighDensity = exp(-viewHeight / rayleighScaleHeight);
    float mieDensity = exp(-viewHeight / mieScaleHeight);

    // Combine phase functions with wavelength-dependent Rayleigh tint and Mie
    // intensity to approximate single scattering along the view direction.
    float rayleighPhase = RayleighPhase(cosSunView);
    float miePhase = MiePhase(cosSunView, 0.76);

    float3 rayleighColor = float3(0.650, 0.570, 0.475);
    float3 scattered = rayleighColor * rayleighDensity * rayleighPhase;
    scattered += LightColor * mieDensity * miePhase * 0.8;

    // Attenuate based on sun elevation to reduce contribution near the horizon.
    float sunAttenuation = saturate(exp(-max(0.0, 1.0 - cosSunUp) * 2.0));
    float3 atmospheric = scattered * sunAttenuation;

    return baseSky + atmospheric;
}

float4 PSMain(VSOutput Input) : SV_Target
{
    float3 viewDir = normalize(Input.WorldPos - CameraPosition);
    float3 skyColor = ApplyAtmosphere(viewDir);
    return float4(skyColor, 1.0);
}
