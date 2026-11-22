#version 430 core

// One triangle covering the viewport, generated from the vertex id alone: no
// vertex buffer is bound.
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4((corner * 2.0) - 1.0, 0.0, 1.0);
}
