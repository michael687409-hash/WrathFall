// Reproject static geometry from device depth. An object pass can replace
// this estimate where it supplies a complete current-to-previous vector.
cbuffer Camera : register(b0) {
    row_major float4x4 ClipToPreviousClip : packoffset(c0);
    uint2 RenderSize : packoffset(c4.x);
    float2 JitterInPixels : packoffset(c4.z);
    float2 DepthRange : packoffset(c5.x);
    uint ResetHistory : packoffset(c5.z);
    uint UseObjectMotion : packoffset(c5.w);
    uint InvertedDepth : packoffset(c6.x);
    uint ComposedTransition : packoffset(c6.y);
    row_major float4x4 ObjectPoseToPreviousClip : packoffset(c7);
};

Texture2D<float> SceneDepth : register(t0);
Texture2D<float4> ObjectMotion : register(t1);
Texture2D<uint> ObjectMotionValid : register(t2);
RWTexture2D<float2> Motion : register(u0);
RWTexture2D<float> UpscalerDepth : register(u1);

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID) {
    uint2 pixel = thread.xy;
    if (any(pixel >= RenderSize)) {
        return;
    }
    if (ComposedTransition) {
        UpscalerDepth[pixel] = InvertedDepth ? 0.0 : 1.0;
        Motion[pixel] = 0;
        return;
    }
    float rawDepth = SceneDepth.Load(int3(pixel, 0));
    bool validDepth = isfinite(rawDepth) && rawDepth >= DepthRange.x && rawDepth <= DepthRange.y;
    float clipDepth = validDepth ? (rawDepth - DepthRange.x) / (DepthRange.y - DepthRange.x)
                                 : (InvertedDepth ? 0.0 : 1.0);
    UpscalerDepth[pixel] = clipDepth;
    if (ResetHistory) {
        Motion[pixel] = 0;
        return;
    }
    if (UseObjectMotion == 1 && ObjectMotionValid.Load(int3(pixel, 0))) {
        Motion[pixel] = ObjectMotion.Load(int3(pixel, 0)).xy;
        return;
    }

    if (!validDepth) {
        Motion[pixel] = 0;
        return;
    }
    float2 pixelToClip = float2(2.0, -2.0) / RenderSize;
    float2 currentClip = (float2(pixel) + 0.5 - JitterInPixels) * pixelToClip
                         + float2(-1.0, 1.0);
    float4 previousClip = mul(float4(currentClip, clipDepth, 1.0), ClipToPreviousClip);
    if (UseObjectMotion == 2) {
        float4 previousPose = ObjectMotion.Load(int3(pixel, 0));
        if (all(isfinite(previousPose)) && previousPose.w > 1.0e-6) {
            previousClip = mul(previousPose, ObjectPoseToPreviousClip);
        }
    }
    const float2 bounds = float2(RenderSize);
    // A point behind the previous lens has no in-frame history.
    float2 velocity = -bounds;
    if (all(isfinite(previousClip)) && previousClip.w > 1.0e-6) {
        velocity = clamp((previousClip.xy / previousClip.w - currentClip) / pixelToClip,
                         -bounds, bounds);
    }
    Motion[pixel] = velocity;
}
