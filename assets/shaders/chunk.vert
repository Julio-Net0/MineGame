#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
in vec2 vertexTexCoord2;

uniform mat4 mvp;

out vec2 fragTexCoord;
out vec4 fragColor;
out float fragLayer;
out float fragOverlayLayer;

void main() {
    fragTexCoord = vertexTexCoord;
    // rgb is the face's biome tint, a is its per-vertex AO brightness.
    fragColor = vertexColor;
    // x is the base texture array layer, y the optional overlay layer composited
    // over it (negative when the face has none).
    fragLayer = vertexTexCoord2.x;
    fragOverlayLayer = vertexTexCoord2.y;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
