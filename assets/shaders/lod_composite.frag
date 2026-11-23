#version 430 core

// Lays the LOD pass's pixels over the frame, blended with (1, 1 - alpha): the
// LOD target is cleared to zero and holds colour premultiplied by coverage,
// so opaque LOD pixels (alpha 1) replace the frame exactly, untouched ones
// leave it exactly as the 1:1 passes drew it, and blended water over nothing
// mixes with the frame behind it.

uniform sampler2D uLodColor;

out vec4 finalColor;

void main() {
    vec4 color = texelFetch(uLodColor, ivec2(gl_FragCoord.xy), 0);
    if (color.a <= 0.0) {
        discard;
    }
    finalColor = color;
}
