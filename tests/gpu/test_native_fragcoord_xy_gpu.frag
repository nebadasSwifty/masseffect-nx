#version 450
layout(location=0) flat in uint index;
layout(set=0,binding=0,std430) buffer Results { uvec4 values[]; } result;
void main() { result.values[index]=floatBitsToUint(gl_FragCoord); }
