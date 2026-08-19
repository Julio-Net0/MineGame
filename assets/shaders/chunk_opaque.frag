#version 330

// Shared with the cutout variant: only the alpha test differs. Any change to the
// sampling, tint, overlay composite or ambient-occlusion handling has to be made
// in both, or opaque and cutout geometry will stop matching.

// OPAQUE variant: no `discard`.
//
// A fragment shader containing `discard` prevents the hardware from treating a
// fragment's depth as final before shading it, so early depth rejection is
// disabled for the whole draw. Solid block faces never need the alpha test, and
// on the low-end target -- where waiting on the GPU is 60% of the frame -- paying
// for it across all solid geometry is the single most expensive thing this
// shader could do.

in vec2 fragTexCoord;
in vec4 fragColor;
in float fragLayer;
in float fragOverlayLayer;

uniform sampler2DArray uBlockArray;

out vec4 finalColor;

// Tint applies per texel while ambient occlusion applies per vertex, so the two
// travel separately rather than pre-multiplied: fragColor.rgb carries the biome
// tint (white when the block declares none) and fragColor.a carries AO
// brightness.
//
// Which texels are tintable is decided by the artist, not guessed from colour.
// A face may name an overlay layer whose alpha marks the tintable region: the
// overlay takes the tint and composites over an untinted base. That is how a
// grass block tints its grass fringe while the dirt behind it — debris specks
// and all — keeps its own colour. No overlay means the whole texel is tintable,
// which is what the grass top and leaves want; untinted blocks carry a white
// tint, making that a no-op for everything else.
void main() {
    vec4 base = texture(uBlockArray, vec3(fragTexCoord, fragLayer));

    vec3 tint = fragColor.rgb;
    float ao = fragColor.a;
    vec3 rgb;

    if (fragOverlayLayer >= 0.0) {
        vec4 overlay = texture(uBlockArray, vec3(fragTexCoord, fragOverlayLayer));
        rgb = mix(base.rgb, overlay.rgb * tint, overlay.a);
    } else {
        rgb = base.rgb * tint;
    }

    // Alpha comes from the base texture alone. AO lives in the vertex alpha and
    // must not reach the blend, or occluded faces would fade out.
    finalColor = vec4(rgb * ao, base.a);
}
