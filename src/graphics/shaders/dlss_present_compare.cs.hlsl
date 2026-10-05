// NVIDIA DLSS upscaling, before presenting: whether the frame the game is
// presenting (at the render resolution) is, in each 8x8 tile, what its main
// render target held when the upscaled picture stopped following it (the
// first copy of it after the HUD) - then the upscaled picture, which has the
// same HUD drawn at the output resolution, can be shown there. Anything drawn
// later (a pause menu over a blurred copy, a fade) makes the tile differ, and
// the frame itself is shown there instead.
//
// Tile result: bit 0 - the same colors, bit 1 - the same with red and blue
// swapped (how the render target's channels end up in the presented frame
// isn't known in advance).

cbuffer XeDlssCompareConstants : register(b0) {
  uint2 xe_dlss_compare_size;
  float xe_dlss_compare_tolerance;
  // Pixels at the top and left edges left out: resolves at a draw resolution
  // scale fill the half-pixel gap there with the next pixels.
  uint xe_dlss_compare_border;
};

// The presented frame, as the presenter reads it (with its swizzle).
Texture2D<float4> xe_dlss_compare_frame : register(t0);
// The render target's copy.
Texture2D<float4> xe_dlss_compare_copy : register(t1);
RWTexture2D<uint> xe_dlss_compare_tiles : register(u0);

groupshared uint xe_dlss_compare_differs;

[numthreads(8, 8, 1)]
void main(uint3 xe_group_id : SV_GroupID, uint3 xe_thread_id : SV_DispatchThreadID,
          uint xe_group_index : SV_GroupIndex) {
  if (xe_group_index == 0u) {
    xe_dlss_compare_differs = 0u;
  }
  GroupMemoryBarrierWithGroupSync();
  [branch] if (all(xe_thread_id.xy < xe_dlss_compare_size) &&
                all(xe_thread_id.xy >= xe_dlss_compare_border)) {
    int3 pixel = int3(xe_thread_id.xy, 0);
    float3 frame = xe_dlss_compare_frame.Load(pixel).rgb;
    float3 copy = xe_dlss_compare_copy.Load(pixel).rgb;
    uint differs = 0u;
    if (any(abs(frame - copy) > xe_dlss_compare_tolerance)) {
      differs |= 1u;
    }
    if (any(abs(frame - copy.bgr) > xe_dlss_compare_tolerance)) {
      differs |= 2u;
    }
    if (differs != 0u) {
      InterlockedOr(xe_dlss_compare_differs, differs);
    }
  }
  GroupMemoryBarrierWithGroupSync();
  if (xe_group_index == 0u) {
    xe_dlss_compare_tiles[xe_group_id.xy] = ~xe_dlss_compare_differs & 3u;
  }
}
