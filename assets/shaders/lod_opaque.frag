#version 430 core

// LOD sections wholly outside the 1:1 chunks: no `discard`, so the pass keeps
// early depth rejection. Shading matches chunk_opaque.frag (tint, overlay,
// ambient occlusion) and must change with it, plus lod_border.frag and
// lod_translucent.frag; on top of it, LOD shaders fade to the sky colour at
// the edge of the LOD (ApplyFog), which the 1:1 chunks never do.
//
// See-through faces (water, ice, glass) are drawn by lod_translucent.frag;
// every face here is opaque, leaves included, so alpha is forced to 1.

in vec2 fragTexCoord;
in vec4 fragColor;
in float fragLayer;
in float fragOverlayLayer;
in vec3 fragProbe;

uniform sampler2DArray uBlockArray;

out vec4 finalColor;

uniform bool uFogEnabled;
// Horizontal distance in blocks where the fog starts and where it reaches
// uFogColor, the sky's clear colour.
uniform vec2 uFogRange;
uniform vec3 uFogColor;

vec3 ApplyFog(vec3 rgb) {
    if (!uFogEnabled) {
        return rgb;
    }
    float fog = clamp((length(fragProbe.xz) - uFogRange.x) / (uFogRange.y - uFogRange.x),
                      0.0, 1.0);
    if (fog <= 0.0) {
        return rgb;
    }
    return mix(rgb, uFogColor, fog);
}

// Cut-out textures (leaves) are drawn solid at LOD distances: a transparent
// texel's own colour is whatever the file holds (often black), so it takes
// the texture's mean colour instead, the alpha-weighted 1x1 mip. Opaque
// texels are untouched.
vec4 SampleBase() {
    vec4 base = texture(uBlockArray, vec3(fragTexCoord, fragLayer));
    if (base.a < 1.0) {
        vec3 mean = textureLod(uBlockArray, vec3(fragTexCoord, fragLayer), 16.0).rgb;
        base.rgb = mix(mean, base.rgb, base.a);
    }
    return base;
}

void main() {
    vec4 base = SampleBase();
    vec3 tint = fragColor.rgb;
    vec3 rgb;
    if (fragOverlayLayer >= 0.0) {
        vec4 overlay = texture(uBlockArray, vec3(fragTexCoord, fragOverlayLayer));
        rgb = mix(base.rgb, overlay.rgb * tint, overlay.a);
    } else {
        rgb = base.rgb * tint;
    }
    finalColor = vec4(ApplyFog(rgb * fragColor.a), 1.0);
}
