#ifndef RESTIR_GI_RESERVOIR_HLSLI
#define RESTIR_GI_RESERVOIR_HLSLI

struct FRestirGIReservoir
{
    float3 SampleRadiance;
    float WeightSum;
    float SelectedWeight;
    uint SampleCount;
    float Padding0;
    float Padding1;
};

FRestirGIReservoir CreateEmptyReservoir()
{
    FRestirGIReservoir Reservoir;
    Reservoir.SampleRadiance = 0.0f.xxx;
    Reservoir.WeightSum = 0.0f;
    Reservoir.SelectedWeight = 1.0f;
    Reservoir.SampleCount = 0u;
    Reservoir.Padding0 = 0.0f;
    Reservoir.Padding1 = 0.0f;
    return Reservoir;
}

#endif
