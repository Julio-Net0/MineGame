#version 430 core

// LOD sections that overlap the 1:1 area: each fragment whose voxel lies in a
// chunk column holding any resolved 1:1 chunk is discarded, at every height,
// so the LOD is strictly the far field and never shows through the 1:1 world
// (for instance chunks the vertical window let go of under the terrain).
// Shading is lod_opaque.frag's and must change with it.
//
// uCoverage holds one bit per chunk: texel (cx & 511, cz & 511), chunk layer
// cy + 64 split over the four words (world anchored, chunk layers -64..63);
// a column counts as 1:1 when any bit is set. Only columns within 255 chunks
// of the camera are consulted, which keeps the toroidal texture unambiguous:
// every 1:1 chunk is far closer than that.

in vec2 fragTexCoord;
in vec4 fragColor;
in float fragLayer;
in float fragOverlayLayer;
in vec3 fragProbe;

uniform sampler2DArray uBlockArray;
uniform usampler2D uCoverage;
uniform ivec3 uCameraBlock;
uniform vec3 uCameraFrac;

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

bool CoveredByChunks() {
    ivec3 block = ivec3(floor(fragProbe + uCameraFrac + vec3(0.5))) + uCameraBlock;
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
