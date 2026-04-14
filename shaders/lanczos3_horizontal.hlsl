// Lanczos-3 horizontal downscale: src_width -> dst_width at src_height
// Input:  SRV t0 -- source texture (proxy backbuffer, k*D)
// Output: UAV u0 -- intermediate texture (D_width x O_height)
//
// Feature: adaptive-dlss-scaling
// Separable pass 1 of 2.

cbuffer Constants : register(b0) {
    uint src_width;
    uint src_height;
    uint dst_width;
    uint dst_height;  // == src_height for horizontal pass
};

SamplerState point_sampler : register(s0);
Texture2D<float4> src_tex : register(t0);
RWTexture2D<float4> dst_tex : register(u0);

static const float PI = 3.14159265358979323846;

float sinc(float x) {
    if (abs(x) < 1e-6) return 1.0;
    float px = PI * x;
    return sin(px) / px;
}

float lanczos3(float x) {
    if (abs(x) >= 3.0) return 0.0;
    return sinc(x) * sinc(x / 3.0);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= dst_width || dtid.y >= src_height) return;

    float scale = float(src_width) / float(dst_width);
    float center = (float(dtid.x) + 0.5) * scale - 0.5;

    float4 color = float4(0, 0, 0, 0);
    float weight_sum = 0.0;

    int start = int(floor(center - 3.0));
    int end   = int(ceil(center + 3.0));

    for (int i = start; i <= end; i++) {
        float s = clamp(float(i), 0.0, float(src_width - 1));
        float w = lanczos3(float(i) - center);
        color += src_tex[uint2(uint(s), dtid.y)] * w;
        weight_sum += w;
    }

    dst_tex[dtid.xy] = color / weight_sum;
}
