#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
in vec2 vertexTexCoord2;

uniform mat4 mvp;

out vec2 fragTexCoord;
out vec4 fragColor;
out float fragLayer;

void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragLayer = vertexTexCoord2.x;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
