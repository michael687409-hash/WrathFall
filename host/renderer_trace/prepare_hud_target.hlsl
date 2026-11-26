Texture2D<float4> UpscaledScene : register(t0);
Texture2D<float4> NativeScene : register(t1);
RWTexture2D<float4> HudTarget : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID) {
    uint width, height;
    HudTarget.GetDimensions(width, height);
    if (thread.x >= width || thread.y >= height) return;

    // Keep native destination alpha for the original HUD blend equations.
    float3 scene = UpscaledScene.Load(int3(thread.xy, 0)).rgb;
    float alpha = NativeScene.Load(int3(thread.xy / 2, 0)).a;
    HudTarget[thread.xy] = float4(scene, alpha);
}

[numthreads(8, 8, 1)]
void CopyTransition(uint3 thread : SV_DispatchThreadID) {
    uint width, height;
    HudTarget.GetDimensions(width, height);
    if (thread.x >= width || thread.y >= height) return;

    // Match the original swap view of the loading card.
    HudTarget[thread.xy] = float4(NativeScene.Load(int3(thread.xy, 0)).bgr, 1.0);
}
