#version 450
layout(push_constant) uniform Plane { vec4 coefficients; } plane;
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  // With a positive 4x4 viewport, these are framebuffer XY coordinates.
  vec2 xy = p * 4.0;
  gl_Position = vec4(p * 2.0 - 1.0,
    plane.coefficients.x+plane.coefficients.y*xy.x+plane.coefficients.z*xy.y,1.0);
}
