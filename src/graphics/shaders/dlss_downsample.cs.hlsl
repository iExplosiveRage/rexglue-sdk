// NVIDIA DLSS upscaling: the upscaled 3D scene taken back to the render
// resolution, in place of the scene the game drew - the frame the game goes on
// with (the HUD at the render resolution, its own copies) is then the
// anti-aliased, unjittered one. A box filter over the source pixels of each
// destination pixel, from 4 bilinear taps.

cbuffer XeDlssDownsampleConstants : register(b0) {
  // Destination (render resolution) size.
  uint2 xe_dlss_downsample_size;
  // Source pixels per destination pixel.
  float2 xe_dlss_downsample_ratio;
  // 1 / the source size.
  float2 xe_dlss_downsample_source_size_inv;
  float2 xe_dlss_downsample_padding;
};

Texture2D<float4> xe_dlss_downsample_source : register(t0);
RWTexture2D<float4> xe_dlss_downsample_dest : register(u0);
SamplerState xe_dlss_downsample_sampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (any(xe_thread_id.xy >= xe_dlss_downsample_size)) {
    return;
  }
  float2 center = (float2(xe_thread_id.xy) + 0.5) * xe_dlss_downsample_ratio;
  float2 offset = 0.25 * xe_dlss_downsample_ratio;
  float2 scale = xe_dlss_downsample_source_size_inv;
  float4 sum =
      xe_dlss_downsample_source.SampleLevel(xe_dlss_downsample_sampler,
                                            (center + float2(-offset.x, -offset.y)) * scale, 0.0);
  sum += xe_dlss_downsample_source.SampleLevel(xe_dlss_downsample_sampler,
                                               (center + float2(offset.x, -offset.y)) * scale, 0.0);
  sum += xe_dlss_downsample_source.SampleLevel(xe_dlss_downsample_sampler,
                                               (center + float2(-offset.x, offset.y)) * scale, 0.0);
  sum += xe_dlss_downsample_source.SampleLevel(xe_dlss_downsample_sampler,
                                               (center + float2(offset.x, offset.y)) * scale, 0.0);
  xe_dlss_downsample_dest[xe_thread_id.xy] = sum * 0.25;
}
