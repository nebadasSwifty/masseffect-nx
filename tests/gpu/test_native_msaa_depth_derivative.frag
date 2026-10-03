#version 450
layout(set=0,binding=1,std430) buffer Readback {
  float depth[32]; uint sample_invocations[2];
} output_data;
void main() {
  // SDK spirv_translator_rb.cpp:1930..2003 reconstructs raster depth from
  // center depth and derivatives before FLOAT24 conversion. No divergence
  // precedes these derivatives. Host standard 2x sample0 is bottom-right;
  // host sample1 (SDK guest0) is top-left.
  float center=gl_FragCoord.z;
  float dx=dFdxFine(center), dy=dFdyFine(center);
  vec2 offset=gl_SampleID==0 ? vec2(0.25) : vec2(-0.25);
  precise float reconstructed=center+(dx*offset.x+dy*offset.y);
  atomicAdd(output_data.sample_invocations[gl_SampleID],1u);
  uint bits=floatBitsToUint(clamp(reconstructed,0.0,1.0)*2.0);
  // The fixtures stay in normalized guest [0.5,1): Float20e4 round-even
  // equals reduction of the IEEE binary32 mantissa from 23 to 20 bits.
  bits=(bits+3u+((bits>>3u)&1u))&~7u;
  gl_FragDepth=uintBitsToFloat(bits)*0.5;
}
