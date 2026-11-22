#version 430 core

// LOD section vertex: the same 8-byte packed vertex as a chunk (see
// chunk.vert), in section-local voxel units, scaled by the section's 2^L.
//
// Positions are made camera-relative in integers before anything becomes a
// float: the section origin and the camera's block are both exact ints, so
// only their (small, near the camera) difference is converted, and sections
// thousands of chunks away keep full float precision. uViewProj is built with
// the camera at the origin; the camera's fractional block offset is the only
// translation left.
layout(location = 0) in uvec2 aPackedVertex;
// Draw index, delivered through the indirect command's baseInstance (divisor
// 1 on an identity buffer), so no shader draw parameters are needed.
layout(location = 1) in uint aSectionIndex;

// xyz: section origin in blocks; w: blocks per voxel (2^L).
layout(std430, binding = 0) readonly buffer SectionMeta {
    ivec4 Sections[];
};

uniform mat4 uViewProj;
uniform ivec3 uCameraBlock;
uniform vec3 uCameraFrac;
uniform int uOverlayLayers[16];

out vec2 fragTexCoord;
out vec4 fragColor;
out float fragLayer;
out float fragOverlayLayer;
// Camera-relative point half a block inside the face's voxel, for the border
// variant's coverage lookup: a face on a chunk boundary belongs to the chunk
// its voxel is in.
out vec3 fragProbe;

const vec3 FACE_NORMALS[6] = vec3[6](
    vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(-1.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));

void main() {
    ivec4 meta = Sections[aSectionIndex];
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
    fragLayer = float(w1 & 4095u);
    uint overlaySlot = (w1 >> 12u) & 15u;
    fragOverlayLayer = (overlaySlot == 0u) ? -1.0
                                           : float(uOverlayLayers[overlaySlot]);

    // Blocks are centred on integers, so a voxel corner sits at -0.5.
    float scale = float(meta.w);
    vec3 relative = vec3(meta.xyz - uCameraBlock) + (localPos * scale) -
                    vec3(0.5) - uCameraFrac;
    uint normal = min((w0 >> 27u) & 7u, 5u);
    fragProbe = relative - (FACE_NORMALS[normal] * 0.5);
    gl_Position = uViewProj * vec4(relative, 1.0);
}
