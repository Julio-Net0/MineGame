#version 430 core

// Rewrites the 1:1 pass's depth into the LOD projection's depth, so LOD
// geometry is depth-tested (with early rejection) against the chunks already
// drawn. Both projections share the view and field of view and differ only in
// near/far, so a pixel's view-axis distance is all that has to carry over:
//   ndc = -m10 + m14 / distance   =>   distance = m14 / (ndc + m10)
// uNear and uLod hold (m10, m14) of each projection. Pixels the 1:1 pass never
// covered stay at the far plane.

uniform sampler2D uDepthNear;
uniform vec2 uNear;
uniform vec2 uLod;

void main() {
    float depth = texelFetch(uDepthNear, ivec2(gl_FragCoord.xy), 0).r;
    if (depth >= 1.0) {
        gl_FragDepth = 1.0;
        return;
    }
    float ndc = (depth * 2.0) - 1.0;
    float distance = uNear.y / (ndc + uNear.x);
    float lodNdc = -uLod.x + (uLod.y / distance);
    gl_FragDepth = clamp((lodNdc * 0.5) + 0.5, 0.0, 1.0);
}
