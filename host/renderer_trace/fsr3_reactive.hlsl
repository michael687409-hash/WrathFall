cbuffer Reprojection : register(b0) {
    uint2 RenderSize;
    float2 JitterDelta;
};

Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> PreviousColor : register(t1);
Texture2D<float2> Motion : register(t2);
RWTexture2D<unorm float> ReactiveMask : register(u0);
SamplerState LinearClamp : register(s0);

float MaxChannel(float3 value) {
    return max(value.r, max(value.g, value.b));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID) {
    uint2 pixel = thread.xy;
    if (any(pixel >= RenderSize)) return;

    float2 previousPixel = float2(pixel) + 0.5 + Motion.Load(int3(pixel, 0)) + JitterDelta;
    if (any(previousPixel < 0.5) || any(previousPixel > float2(RenderSize) - 0.5)) {
        ReactiveMask[pixel] = 0.0;
        return;
    }

    float3 reference = PreviousColor.SampleLevel(LinearClamp, previousPixel / RenderSize, 0).rgb;
    int2 p = int2(pixel);
    float3 current = CurrentColor.Load(int3(p, 0)).rgb;
    float3 low = current;
    float3 high = current;
    static const int2 offsets[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    [unroll] for (uint i = 0; i < 4; ++i) {
        int2 neighbor = clamp(p + offsets[i], int2(0, 0), int2(RenderSize) - 1);
        float3 sample = CurrentColor.Load(int3(neighbor, 0)).rgb;
        low = min(low, sample);
        high = max(high, sample);
    }
    // Tolerate ordinary sub-pixel resampling around still, high-contrast edges.
    // Old color outside the current range is strong evidence of unreliable history.
    float outside = MaxChannel(max(low - reference, reference - high));
    float center = MaxChannel(abs(current - reference) - (high - low) * 0.45);
    float threshold = 0.018 + 0.065 * MaxChannel(max(reference, current));
    ReactiveMask[pixel] = 0.85 * saturate((max(outside, center) - threshold) / 0.15);
}
