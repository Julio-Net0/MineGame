#version 330

in vec2 fragTexCoord;
in vec4 fragColor;
in float fragLayer;

uniform sampler2DArray uBlockArray;

out vec4 finalColor;

void main() {
    vec4 texColor = texture(uBlockArray, vec3(fragTexCoord, fragLayer));
    finalColor = texColor * fragColor;
    if (finalColor.a < 0.1) discard;
}
