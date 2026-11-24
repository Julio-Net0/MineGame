#version 430 core

// Rewrites the 1:1 pass's depth into the LOD projection's depth, so LOD
// geometry is depth-tested (with early rejection) against the chunks already
// drawn. Both projections share the view and field of view and differ only in
// near/far, so a pixel's view-axis distance is all that has to carry over:
//   ndc = -m10 + m14 / distance   =>   distance = m14 / (ndc + m10)
// uNear and uLod hold (m10, m14) of each projection. Pixels the 1:1 pass never
// covered stay at the far end of the LOD depth.
//
// With uReversed the LOD projection is reversed-Z with an infinite far plane
// (m10 = 0, m14 = near) and the LOD passes run with a [0, 1] clip range, so
// the LOD depth is near / distance: 1 at the near plane, 0 at infinity, and
// sky pixels get 0.

uniform sampler2D uDepthNear;
uniform vec2 uNear;
uniform vec2 uLod;
uniform bool uReversed;

void main() {
    float depth = texelFetch(uDepthNear, ivec2(gl_FragCoord.xy), 0).r;
    if (depth >= 1.0) {
        gl_FragDepth = uReversed ? 0.0 : 1.0;
        return;
    }
    float ndc = (depth * 2.0) - 1.0;
    float distance = uNear.y / (ndc + uNear.x);
    float lodNdc = -uLod.x + (uLod.y / distance);
    gl_FragDepth = uReversed ? clamp(lodNdc, 0.0, 1.0)
                             : clamp((lodNdc * 0.5) + 0.5, 0.0, 1.0);
}
