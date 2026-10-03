#version 450
layout(set=0,binding=3,std430) readonly buffer Patterns { uvec2 records[]; } patterns;
layout(push_constant) uniform Parameters {
  uvec4 dimensions; // current width,height,observer offset,pattern offset
  uvec4 tiles; uvec4 transfer; uvec4 reserved;
} p;
void main() {
  uvec2 pixel=uvec2(gl_FragCoord.xy);
  uint index=p.dimensions.w+(pixel.y*p.dimensions.x+pixel.x)*2u+uint(gl_SampleID);
  uvec2 value=patterns.records[index];
  uint mask=p.transfer.y;
  if(mask!=0u&&(value.y&mask)==0u)discard;
  gl_FragDepth=uintBitsToFloat(value.x);
}
