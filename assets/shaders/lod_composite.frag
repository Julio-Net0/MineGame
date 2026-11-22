#version 430 core

// Copies the LOD pass's pixels onto the frame: the LOD target is cleared to
// zero alpha and LOD fragments write alpha 1, so every other pixel of the
// frame is left exactly as the 1:1 passes drew it.

uniform sampler2D uLodColor;

out vec4 finalColor;

void main() {
    vec4 color = texelFetch(uLodColor, ivec2(gl_FragCoord.xy), 0);
    if (color.a < 0.5) {
        discard;
    }
    finalColor = vec4(color.rgb, 1.0);
}
