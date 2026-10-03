#version 450
// No explicit FragDepth: the production SPIR-V transform must add raster depth.
// Preserve this original SampleId / atomic use while adding its own prologue.
layout(set=0,binding=1,std430) buffer Readback {
  float depth[32]; uint sample_invocations[2];
} output_data;
void main() {
  atomicAdd(output_data.sample_invocations[gl_SampleID],1u);
}
