#ifndef RESTIR_GI_NEW_RESERVOIR_HLSLI
#define RESTIR_GI_NEW_RESERVOIR_HLSLI

struct FRestirGINewSample
{
    float3 Radiance;
    uint RayDirection;
};

struct FRestirGINewReservoir
{
    FRestirGINewSample Sample;
    float SumWeight;
    float M;
    float W;
};

float RestirGINewLuminance(float3 Color)
{
    return dot(max(Color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}

float RestirGINewTarget(float3 Radiance)
{
    return RestirGINewLuminance(Radiance);
}

bool RestirGINewUpdate(inout FRestirGINewReservoir Reservoir, FRestirGINewSample Candidate, float CandidateWeight, float RandomValue)
{
    if (!isfinite(CandidateWeight) || CandidateWeight <= 0.0f)
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

bool RestirGINewMerge(inout FRestirGINewReservoir Reservoir, FRestirGINewReservoir Other, float TargetWeight, float RandomValue)
{
    const float PreviousM = Reservoir.M;
    const float Weighted = TargetWeight * Other.W * Other.M;
    const bool bUpdated = RestirGINewUpdate(Reservoir, Other.Sample, Weighted, RandomValue);
    Reservoir.M = PreviousM + Other.M;
    return bUpdated;
}

#endif
