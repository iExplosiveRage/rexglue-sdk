// NVIDIA DLSS upscaling: the picture presented at the output resolution - the
// upscaled one (the 3D scene from DLSS, the HUD drawn over it at the output
// resolution) in the 8x8 tiles of the frame where it's what the game presents
// (dlss_present_compare), the game's own frame (at the render resolution)
// stretched elsewhere and in frames without an upscaled picture.

cbuffer XeDlssPresentConstants : register(b0) {
  uint2 xe_dlss_present_size;
  // The game's frame: its size (it may be in a larger texture), and frame
  // pixels per output pixel.
  uint2 xe_dlss_present_frame_size;
  float2 xe_dlss_present_frame_ratio;
  // 1 / the size of the frame's texture.
  float2 xe_dlss_present_frame_texture_size_inv;
  // 0 - only the frame, 1 - the upscaled picture where the tiles allow,
  // 2 - the same, the frame's tiles tinted red (dlss_debug_view 3).
  uint xe_dlss_present_mode;
  uint3 xe_dlss_present_padding;
};

Texture2D<float4> xe_dlss_present_upscaled : register(t0);
Texture2D<float4> xe_dlss_present_frame : register(t1);
Texture2D<uint> xe_dlss_present_tiles : register(t2);
RWTexture2D<float4> xe_dlss_present_dest : register(u0);
SamplerState xe_dlss_present_sampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (any(xe_thread_id.xy >= xe_dlss_present_size)) {
    return;
  }
  float2 frame_position = (float2(xe_thread_id.xy) + 0.5) * xe_dlss_present_frame_ratio;
  uint tile = 0u;
  [branch] if (xe_dlss_present_mode != 0u) {
    uint2 frame_pixel = min(uint2(frame_position), xe_dlss_present_frame_size - 1u);
    tile = xe_dlss_present_tiles.Load(int3(frame_pixel >> 3u, 0));
  }
  float3 color;
  [branch] if (tile != 0u) {
    color = xe_dlss_present_upscaled.Load(int3(xe_thread_id.xy, 0)).rgb;
    if (!(tile & 1u)) {
      color = color.bgr;
    }
  } else {
    float2 frame_size = float2(xe_dlss_present_frame_size);
    float2 uv = clamp(frame_position, 0.5, frame_size - 0.5) *
                xe_dlss_present_frame_texture_size_inv;
    color = xe_dlss_present_frame.SampleLevel(xe_dlss_present_sampler, uv, 0.0).rgb;
    if (xe_dlss_present_mode == 2u) {
      color = lerp(color, float3(1.0, 0.0, 0.0), 0.35);
    }
  }
  xe_dlss_present_dest[xe_thread_id.xy] = float4(color, 1.0);
}
