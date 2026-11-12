#version 330

// Debug lines and cubes: world-space positions and a flat per-vertex colour.
in vec3 vertexPosition;
in vec4 vertexColor;

uniform mat4 mvp;

out vec4 fragColor;

void main() {
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
