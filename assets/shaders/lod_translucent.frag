#version 430 core

// See-through LOD faces (water, ice, glass), drawn after every opaque LOD face
// with depth testing but no depth writes, blended like the 1:1 translucent
// pass: chunk_cutout.frag's shading and alpha test, the sky fog of
// lod_opaque.frag, and lod_border.frag's coverage test, since the same pass
// draws sections inside and beside the 1:1 area (a face in a 1:1 column is
// left to the chunks). Must change with those three.

in vec2 fragTexCoord;
in vec4 fragColor;
in float fragLayer;
in float fragOverlayLayer;
in vec3 fragProbe;
in vec3 fragCutProbe;

uniform sampler2DArray uBlockArray;
uniform usampler2D uCoverage;
uniform ivec3 uCameraBlock;
uniform vec3 uCameraFrac;
uniform float uNearFarReciprocal;

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

bool CoveredByChunks() {
    if (gl_FragCoord.w <= uNearFarReciprocal) {
        return false;
    }
    ivec3 block = ivec3(floor(fragCutProbe + uCameraFrac + vec3(0.5))) + uCameraBlock;
    ivec2 column = ivec2(floor(vec2(block.xz) / 16.0));
    ivec2 cameraColumn = ivec2(floor(vec2(uCameraBlock.xz) / 16.0));
    ivec2 offset = abs(column - cameraColumn);
    if (max(offset.x, offset.y) > 255) {
        return false;
    }
    uvec4 words = texelFetch(uCoverage, ivec2(column.x & 511, column.y & 511), 0);
    return any(notEqual(words, uvec4(0u)));
}

void main() {
    if (CoveredByChunks()) {
        discard;
    }
    vec4 base = texture(uBlockArray, vec3(fragTexCoord, fragLayer));
    vec3 tint = fragColor.rgb;
    vec3 rgb;
    if (fragOverlayLayer >= 0.0) {
        vec4 overlay = texture(uBlockArray, vec3(fragTexCoord, fragOverlayLayer));
        rgb = mix(base.rgb, overlay.rgb * tint, overlay.a);
    } else {
        rgb = base.rgb * tint;
    }
    if (base.a < 0.1) {
        discard;
    }
    finalColor = vec4(ApplyFog(rgb * fragColor.a), base.a);
}
