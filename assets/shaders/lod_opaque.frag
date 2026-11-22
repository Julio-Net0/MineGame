#version 430 core

// LOD sections wholly outside the 1:1 chunks: no `discard`, so the pass keeps
// early depth rejection. Shading matches chunk_opaque.frag (tint, overlay,
// ambient occlusion) and must change with it, plus lod_border.frag.
//
// Every LOD face is drawn opaque, including the ones a chunk would blend
// (water, leaves, glass): at LOD distances their colour is what matters, and
// alpha is forced to 1 so the frame's blending leaves them solid.

in vec2 fragTexCoord;
in vec4 fragColor;
in float fragLayer;
in float fragOverlayLayer;
in vec3 fragProbe;

uniform sampler2DArray uBlockArray;

out vec4 finalColor;

void main() {
    vec4 base = texture(uBlockArray, vec3(fragTexCoord, fragLayer));
    vec3 tint = fragColor.rgb;
    vec3 rgb;
    if (fragOverlayLayer >= 0.0) {
        vec4 overlay = texture(uBlockArray, vec3(fragTexCoord, fragOverlayLayer));
        rgb = mix(base.rgb, overlay.rgb * tint, overlay.a);
    } else {
        rgb = base.rgb * tint;
    }
    finalColor = vec4(rgb * fragColor.a, 1.0);
}
