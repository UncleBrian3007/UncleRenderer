#ifndef RESTIR_GI_RESERVOIR_HLSLI
#define RESTIR_GI_RESERVOIR_HLSLI

struct FRestirGISample
{
    float3 Radiance;
    uint RayDirection;
};

struct FRestirGIReservoir
{
    FRestirGISample Sample;
    float SumWeight;
    float M;
    float W;
};

float RestirGILuminance(float3 Color)
{
    return dot(max(Color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}

float RestirGITarget(float3 Radiance)
{
    return RestirGILuminance(Radiance);
}

bool RestirGIUpdate(inout FRestirGIReservoir Reservoir, FRestirGISample Candidate, float CandidateWeight, float RandomValue)
{
    if (!isfinite(CandidateWeight))// || CandidateWeight <= 0.0f)
    {
        return false;
    }

    Reservoir.SumWeight += CandidateWeight;
    Reservoir.M += 1.0f;

    // Samples with larger weights are more likely to be selected.
    const float Probability = CandidateWeight / max(Reservoir.SumWeight, 1e-5f);
    if (RandomValue < Probability)
    {
        Reservoir.Sample = Candidate;
        return true;
    }

    return false;
}

bool RestirGIMerge(inout FRestirGIReservoir Reservoir, FRestirGIReservoir Other, float TargetWeight, float RandomValue)
{
    const float PreviousM = Reservoir.M;
    const float Weighted = TargetWeight * Other.W * Other.M;
    const bool bUpdated = RestirGIUpdate(Reservoir, Other.Sample, Weighted, RandomValue);
    Reservoir.M = PreviousM + Other.M;
    return bUpdated;
}

#endif
