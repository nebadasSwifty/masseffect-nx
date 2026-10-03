#version 450
layout(set=0,binding=1,std430) buffer Readback {
  float depth[32]; uint sample_invocations[2];
} output_data;
void main() {
  // Deliberately naive NEGATIVE regression: reading SampleId and enabling
  // sample-rate shading does NOT prove FragCoord.z is per-sample. The actual
  // tested backend invokes both samples but writes center Z here. Operations
  // are retained unchanged to preserve the failed assumption as a fixture.
  atomicAdd(output_data.sample_invocations[gl_SampleID],1u);
  // Test plane stays in guest [0.5,1), where Float20e4 is normalized and
  // nearest-even conversion is exactly a 23->20 mantissa-bit reduction.
  uint bits=floatBitsToUint(gl_FragCoord.z*2.0);
  bits=(bits+3u+((bits>>3u)&1u))&~7u;
  gl_FragDepth=uintBitsToFloat(bits)*0.5;
}
