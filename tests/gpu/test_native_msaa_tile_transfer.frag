#version 450
layout(set=0,binding=0) uniform sampler2DMS source_depth;
layout(set=0,binding=2) uniform usampler2DMS source_stencil;
layout(push_constant) uniform Parameters {
  uvec4 dimensions;
  uvec4 tiles; // source pitch,destination pitch,source start,destination start
  uvec4 transfer; // count,stencil bit mask,negative mode,unused
  uvec4 reserved;
} p;
void main() {
  uvec2 pixel=uvec2(gl_FragCoord.xy);
  // 2x physical words are vertical. Guest sample0 is host sample1.
  uint guest_sample=uint(gl_SampleID)^1u;
  uvec2 physical=uvec2(pixel.x,pixel.y*2u+guest_sample);
  uint destination_tile=(physical.y/16u)*p.tiles.y+physical.x/80u;
  if(destination_tile<p.tiles.w||destination_tile-p.tiles.w>=p.transfer.x)discard;
  uint source_tile=p.tiles.z+destination_tile-p.tiles.w;
  uvec2 source_physical=uvec2((source_tile%p.tiles.x)*80u+physical.x%80u,
    (source_tile/p.tiles.x)*16u+physical.y%16u);
  ivec2 source_pixel=ivec2(source_physical.x,source_physical.y/2u);
  uint source_host_sample=(source_physical.y&1u)^1u;
  // Deliberate negative controls; CPU expected mapping is NEVER changed.
  if(p.transfer.z==1u)source_host_sample^=1u;
  if(p.transfer.z==2u)source_host_sample=1u;
  if(p.transfer.z==3u&&pixel.x>=20u&&pixel.x<24u&&pixel.y==9u)discard;
  uint stencil=texelFetch(source_stencil,source_pixel,int(source_host_sample)).r;
  if(p.transfer.y!=0u&&(stencil&p.transfer.y)==0u)discard;
  // Same-format depth import: no pack/unpack/quantization/arithmetic.
  gl_FragDepth=texelFetch(source_depth,source_pixel,int(source_host_sample)).r;
}
