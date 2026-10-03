#version 450
layout(location=0) flat out uint index;
void main() {
  index=uint(gl_VertexIndex);
  vec2 pixel=vec2(index%8u,index/8u)+0.5;
  gl_Position=vec4(pixel/4.0-1.0,0.375,1.0);
  gl_PointSize=1.0;
}
