#version 330

// UI draw list: screen-pixel positions, GUI atlas or block-icon atlas UVs, and
// a per-vertex colour that tints the texel (solid fills sample a white texel).
in vec2 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;

uniform mat4 mvp;

out vec2 fragTexCoord;
out vec4 fragColor;

void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 0.0, 1.0);
}
