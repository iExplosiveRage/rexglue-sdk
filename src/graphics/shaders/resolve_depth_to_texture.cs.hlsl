// Direct resolve (gpu_direct_resolve) of a kD24S8 depth render target into the
// k_24_8 (R32_FLOAT) texture that samples the resolved depth: exactly what the
// depth dump (depth * 0xFFFFFF rounded to the nearest even integer), the fast
// 32bpp resolve copy and the depth texture load ((d + (d >> 23)) * 2^-24) give.
// The stencil goes to a separate R8_UINT texture, for writing the resolved
// words (depth << 8 | stencil) to memory later if something else reads them
// (texture_store_32bpb_tiled with XE_STORE_DEPTH24).
// See resolve_rt_to_texture_8888.cs.hlsl for the constants.
//
// Build: fxc /nologo /T cs_5_1 /E main /O3 /Vn resolve_depth_to_texture_cs
//            /Fh bytecode/d3d12_5_1/resolve_depth_to_texture_cs.h
//            resolve_depth_to_texture.cs.hlsl

cbuffer xe_direct_resolve_constants : register(b0) {
  uint2 xe_direct_origin;
  uint2 xe_direct_size;
  uint xe_direct_swap_red_blue;
  uint2 xe_direct_fill;
};

Texture2D<float> xe_direct_depth : register(t0);
Texture2D<uint2> xe_direct_stencil : register(t1);
RWTexture2D<float> xe_direct_dest : register(u0);
RWTexture2D<uint> xe_direct_stencil_dest : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  if (any(xe_thread_id.xy >= xe_direct_size)) {
    return;
  }
  int3 source = int3(xe_direct_origin + max(xe_thread_id.xy, xe_direct_fill), 0);
  uint depth = uint(round(xe_direct_depth.Load(source) * 16777215.0f));
  xe_direct_dest[xe_thread_id.xy] = float(depth + (depth >> 23u)) * 5.9604644775390625e-8f;
  xe_direct_stencil_dest[xe_thread_id.xy] = xe_direct_stencil.Load(source).y & 0xFFu;
}
