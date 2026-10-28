/*
 * ps3recomp - SMAA 1x shaders for the D3D12 backend (RSX_AA=2).
 *
 * Enhanced Subpixel Morphological Antialiasing by Jorge Jimenez, Jose I.
 * Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez, from their
 * reference implementation (third_party/smaa, MIT): SMAA.hlsl, embedded as
 * rsx_smaa_src (rsx_smaa_src.h), and its two lookup textures. This file is
 * the glue around it, under the project's MIT licence: the porting macros
 * SMAA.hlsl asks for and the three passes' entry points.
 *
 * The backend compiles rsx_smaa_head + rsx_smaa_src + rsx_smaa_tail once per
 * entry point. Every pass has the same bindings: t0-t2 one table, b0 =
 * { 1/width, 1/height, width, height } of the target (SMAA_RT_METRICS), s0 a
 * bilinear clamp sampler and s1 a point clamp one.
 *   1. smaa_edge: luma edges of t0 (the colour) into an R8G8 target, cleared
 *      first (pixels without an edge are discarded).
 *   2. smaa_weight: blending weights from t0 (the edges), t1 (AreaTex, R8G8)
 *      and t2 (SearchTex, R8) into an RGBA8 target.
 *   3. smaa_blend: t0 (the colour) blended by t1 (the weights) into the
 *      target. The pipeline writes RGB only, so the target's alpha is kept.
 */
#pragma once

static const char rsx_smaa_head[] =
    "#define SMAA_CUSTOM_SL 1\n"
    "#define SMAA_PRESET_HIGH 1\n"
    "cbuffer SmaaMetrics : register(b0) { float4 smaa_rt_metrics; };\n"
    "#define SMAA_RT_METRICS smaa_rt_metrics\n"
    "SamplerState smaa_lin : register(s0);   // bilinear, clamp\n"
    "SamplerState smaa_pt : register(s1);    // point, clamp\n"
    "#define SMAATexture2D(tex) Texture2D tex\n"
    "#define SMAATexturePass2D(tex) tex\n"
    "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(smaa_lin, coord, 0)\n"
    "#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(smaa_pt, coord, 0)\n"
    "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(smaa_lin, coord, 0, offset)\n"
    "#define SMAASample(tex, coord) tex.Sample(smaa_lin, coord)\n"
    "#define SMAASamplePoint(tex, coord) tex.Sample(smaa_pt, coord)\n"
    "#define SMAASampleOffset(tex, coord, offset) tex.Sample(smaa_lin, coord, offset)\n"
    "#define SMAA_FLATTEN [flatten]\n"
    "#define SMAA_BRANCH [branch]\n"
    "#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex\n"
    "#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)\n"
    "#line 1 \"SMAA.hlsl\"\n";

static const char rsx_smaa_tail[] =
    "#line 1 \"rsx_smaa.h\"\n"
    "Texture2D smaa_t0 : register(t0);\n"
    "Texture2D smaa_t1 : register(t1);\n"
    "Texture2D smaa_t2 : register(t2);\n"
    "\n"
    "// A triangle covering the target, from SV_VertexID 0-2 (as rsx_fxaa_vs), and\n"
    "// its texture coordinate: (0,0) at the top left, (1,1) at the bottom right.\n"
    "float2 smaa_corner(uint id, out float4 pos)\n"
    "{\n"
    "    const float2 t = float2((id << 1) & 2, id & 2);\n"
    "    pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    return t;\n"
    "}\n"
    "\n"
    "void smaa_edge_vs(uint id : SV_VertexID, out float4 pos : SV_Position,\n"
    "                  out float2 uv : TEXCOORD0, out float4 offset[3] : TEXCOORD1)\n"
    "{\n"
    "    uv = smaa_corner(id, pos);\n"
    "    SMAAEdgeDetectionVS(uv, offset);\n"
    "}\n"
    "float2 smaa_edge_ps(float4 pos : SV_Position, float2 uv : TEXCOORD0,\n"
    "                    float4 offset[3] : TEXCOORD1) : SV_Target\n"
    "{\n"
    "    return SMAALumaEdgeDetectionPS(uv, offset, smaa_t0);\n"
    "}\n"
    "\n"
    "void smaa_weight_vs(uint id : SV_VertexID, out float4 pos : SV_Position,\n"
    "                    out float2 uv : TEXCOORD0, out float2 pix : TEXCOORD1,\n"
    "                    out float4 offset[3] : TEXCOORD2)\n"
    "{\n"
    "    uv = smaa_corner(id, pos);\n"
    "    SMAABlendingWeightCalculationVS(uv, pix, offset);\n"
    "}\n"
    "float4 smaa_weight_ps(float4 pos : SV_Position, float2 uv : TEXCOORD0,\n"
    "                      float2 pix : TEXCOORD1, float4 offset[3] : TEXCOORD2) : SV_Target\n"
    "{\n"
    "    return SMAABlendingWeightCalculationPS(uv, pix, offset, smaa_t0, smaa_t1, smaa_t2,\n"
    "                                           float4(0.0, 0.0, 0.0, 0.0));   // SMAA 1x\n"
    "}\n"
    "\n"
    "void smaa_blend_vs(uint id : SV_VertexID, out float4 pos : SV_Position,\n"
    "                   out float2 uv : TEXCOORD0, out float4 offset : TEXCOORD1)\n"
    "{\n"
    "    uv = smaa_corner(id, pos);\n"
    "    SMAANeighborhoodBlendingVS(uv, offset);\n"
    "}\n"
    "float4 smaa_blend_ps(float4 pos : SV_Position, float2 uv : TEXCOORD0,\n"
    "                     float4 offset : TEXCOORD1) : SV_Target\n"
    "{\n"
    "    return SMAANeighborhoodBlendingPS(uv, offset, smaa_t0, smaa_t1);\n"
    "}\n";
