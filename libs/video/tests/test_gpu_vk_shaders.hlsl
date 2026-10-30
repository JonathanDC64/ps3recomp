// ps3recomp - test_gpu_vk's own shaders (not a game's), HLSL for glslangValidator.
//
// test_gpu_vk_spv.h holds them compiled; it says how it was made. The
// bindings are the Vulkan backend's rules (gpu_vk_internal.h), written out:
// set 0 has b<n> at binding n, a raw t<n> at 16 + n and a static s<n> at
// 48 + n; the layout's tables are sets 1, 2, ... in slot order, t<j> at
// 16 + j and s<j> at 48 + j; CONST32 slots are the push constant block.
// The last stage before the rasterizer flips Y (--iy), as the SPIR-V target
// will: the vertex shaders, or the geometry shader where there is one.

#if defined(GUEST_VS)
// The guest pipelines' six-slot layout: b0 (vertex), t0-t15 (pixel), b1
// (pixel), s0-s15 (pixel), raw t20 (vertex), b2 x1 (vertex).
[[vk::binding(0, 0)]] cbuffer C0 : register(b0) { float4 offs; };
struct PC { uint k; };
[[vk::push_constant]] ConstantBuffer<PC> pc;
[[vk::binding(36, 0)]] ByteAddressBuffer raw : register(t20);
struct VI {
    [[vk::location(0)]] float4 p : ATTR0;
    [[vk::location(1)]] float4 c : ATTR1;
};
struct VO {
    float4 p : SV_Position;
    [[vk::location(0)]] float4 c : COLOR0;
};
VO main(VI i)
{
    VO o;
    o.p = i.p + offs;
    o.c = i.c * asfloat(raw.Load(pc.k * 4));
    return o;
}
#endif

#if defined(GUEST_PS)
// t1 is an empty entry: it reads zero, so it adds nothing.
[[vk::binding(1, 0)]] cbuffer C1 : register(b1) { float4 tint; };
[[vk::binding(16, 1)]] Texture2D t0 : register(t0);
[[vk::binding(17, 1)]] Texture2D t1 : register(t1);
[[vk::binding(48, 2)]] SamplerState s0 : register(s0);
float4 main(float4 p : SV_Position, [[vk::location(0)]] float4 c : COLOR0) : SV_Target
{
    return c * tint * t0.Sample(s0, float2(0.5, 0.5)) + t1.Sample(s0, float2(0.5, 0.5));
}
#endif

#if defined(GUEST_GS)
// A pass-through geometry shader on the guest layout (GPU_CAP_GS).
struct GV {
    float4 p : SV_Position;
    [[vk::location(0)]] float4 c : COLOR0;
};
[maxvertexcount(3)]
void main(triangle GV i[3], inout TriangleStream<GV> s)
{
    for (int k = 0; k < 3; k++) s.Append(i[k]);
}
#endif

#if defined(POST_VS)
// A post pass: one triangle over the target from SV_VertexID, no vertices.
struct PV {
    float4 p : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};
PV main(uint id : SV_VertexID)
{
    PV o;
    const float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
#endif

#if defined(POST_PS)
// The post layout: a one-entry table t0 (set 1) and a static linear clamp s0.
[[vk::binding(16, 1)]] Texture2D t0 : register(t0);
[[vk::binding(48, 0)]] SamplerState s0 : register(s0);
float4 main(float4 p : SV_Position, [[vk::location(0)]] float2 uv : TEXCOORD0) : SV_Target
{
    return t0.Sample(s0, uv);
}
#endif
