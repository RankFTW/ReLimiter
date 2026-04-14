// Lanczos-3 vertical downscale: src_height -> dst_height at dst_width
// Input:  SRV t0 -- intermediate texture (D_width x O_height)
// Output: UAV u0 -- destination texture (D_width x D_height)
//
// Feature: adaptive-dlss-scaling
// Separable pass 2 of 2.

cbuffer Constants : register(b0) {
    uint src_width;   // == dst_width for vertical pass
    uint src_height;
    uint dst_width;
    uint dst_height;
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
    if (dtid.x >= dst_width || dtid.y >= dst_height) return;

    float scale = float(src_height) / float(dst_height);
    float center = (float(dtid.y) + 0.5) * scale - 0.5;

    float4 color = float4(0, 0, 0, 0);
    float weight_sum = 0.0;

    int start = int(floor(center - 3.0));
    int end   = int(ceil(center + 3.0));

    for (int i = start; i <= end; i++) {
        float s = clamp(float(i), 0.0, float(src_height - 1));
        float w = lanczos3(float(i) - center);
        color += src_tex[uint2(dtid.x, uint(s))] * w;
        weight_sum += w;
    }

    dst_tex[dtid.xy] = color / weight_sum;
}
