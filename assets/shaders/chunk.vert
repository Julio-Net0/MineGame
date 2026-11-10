#version 330

// 8-byte packed chunk vertex:
//   x (Word0): X(5b) | Y(5b) | Z(5b) | U(5b) | V(5b) | AO(2b) | Normal(3b)
//   y (Word1): TexLayer(8b) | OverlayLayer(8b, 255 = none) | TintRGB565(16b)
layout(location = 0) in uvec2 aPackedVertex;

uniform mat4 mvp;
uniform vec3 uChunkOrigin;

out vec2 fragTexCoord;
out vec4 fragColor;
out float fragLayer;
out float fragOverlayLayer;

void main() {
    uint w0 = aPackedVertex.x;
    uint w1 = aPackedVertex.y;

    vec3 localPos = vec3(
        float( w0         & 31u),
        float((w0 >>  5u) & 31u),
        float((w0 >> 10u) & 31u)
    );

    fragTexCoord = vec2(
        float((w0 >> 15u) & 31u),
        float((w0 >> 20u) & 31u)
    );

    uint aoLevel = (w0 >> 25u) & 3u;
    float ao = float(255u - (aoLevel * 45u)) * (1.0 / 255.0);

    vec3 tint = vec3(
        float((w1 >> 27u) & 31u) * (1.0 / 31.0),
        float((w1 >> 21u) & 63u) * (1.0 / 63.0),
        float((w1 >> 16u) & 31u) * (1.0 / 31.0)
    );
    fragColor = vec4(tint, ao);

    fragLayer = float(w1 & 255u);
    uint overlayRaw = (w1 >> 8u) & 255u;
    fragOverlayLayer = (overlayRaw == 255u) ? -1.0 : float(overlayRaw);

    gl_Position = mvp * vec4(uChunkOrigin + localPos, 1.0);
}
