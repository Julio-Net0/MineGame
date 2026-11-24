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
// Skirt faces (word 0 bit 30) close the cut the border pass makes at the 1:1
// columns; every other pass collapses them.
uniform bool uDrawSkirts;
// Sea faces (word 0 bit 31): water and sea ice. A level's sea ends on the top
// of the voxel holding the sea block row, 2^L - 1 blocks above the 1:1 sea's
// top; that line moves onto it, so the LOD sea meets the 1:1 sea (and every
// level meets every other) at one height.
uniform int uSeaLevel;

out vec2 fragTexCoord;
out vec4 fragColor;
out float fragLayer;
out float fragOverlayLayer;
// Camera-relative point half a block inside the face's voxel, for the border
// variant's coverage lookup: a face on a chunk boundary belongs to the chunk
// its voxel is in.
out vec3 fragProbe;
// fragProbe moved PROBE_INSET into the quad along its two edges' axes, for
// the 1:1 cut: a quad edge on a chunk column plane puts its probes on the
// plane, where interpolation can carry a fragment's probe a hair into the
// next column, so the cut would take or keep a row of pixels by rounding.
// Inset, every fragment is judged by its own quad's side.
out vec3 fragCutProbe;
// A skirt's neighbour, half a block across the face (inset the same way): the
// skirt is kept only where that voxel's column is 1:1.
out vec3 fragSkirtProbe;
flat out uint fragSkirt;

const float PROBE_INSET = 1.0 / 64.0;

const vec3 FACE_NORMALS[6] = vec3[6](
    vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(-1.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));

void main() {
    ivec4 meta = Sections[aSectionIndex];
    uint w0 = aPackedVertex.x;
    uint w1 = aPackedVertex.y;
    fragSkirt = (w0 >> 30u) & 1u;
    if (fragSkirt != 0u && !uDrawSkirts) {
        // All of a quad's vertices land on one point: nothing is rasterised.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

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
    // Level 1 keeps the chunks' ambient occlusion, so the 1:1 seam shows no
    // brightness step. Coarser voxels are 4 to 128 blocks wide, where full
    // corner shading turns into dark blotches: it fades by a fifth per level,
    // keeping at least 30% for relief.
    if (meta.w > 2) {
        float strength = clamp(1.0 - 0.2 * (log2(float(meta.w)) - 1.0), 0.3, 1.0);
        ao = 1.0 - (1.0 - ao) * strength;
    }
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
    if (((w0 >> 31u) & 1u) != 0u) {
        // Block row whose bottom this vertex's voxel line is.
        int lineY = meta.y + (int(localPos.y) * meta.w);
        int seaVoxelTop = (uSeaLevel >> findLSB(meta.w) << findLSB(meta.w)) + meta.w;
        if (lineY == seaVoxelTop) {
            relative.y -= float(seaVoxelTop - (uSeaLevel + 1));
        }
    }
    uint normal = min((w0 >> 27u) & 7u, 5u);
    fragProbe = relative - (FACE_NORMALS[normal] * 0.5);
    // Towards the quad's inside along x and z, from which end of the face's
    // tile axes this vertex sits on (see AddGreedyFaceToMeshBuilder).
    float alongU = (((w0 >> 15u) & 31u) == 0u) ? 1.0 : -1.0;
    float alongV = (((w0 >> 20u) & 31u) == 0u) ? 1.0 : -1.0;
    vec3 inward = vec3(0.0);
    switch (normal) {
        case 0u: inward = vec3(alongU, 0.0, alongV); break;
        case 1u: inward = vec3(alongU, 0.0, -alongV); break;
        case 2u: inward = vec3(0.0, 0.0, alongU); break;
        case 3u: inward = vec3(0.0, 0.0, -alongU); break;
        case 4u: inward = vec3(alongU, 0.0, 0.0); break;
        default: inward = vec3(-alongU, 0.0, 0.0); break;
    }
    fragCutProbe = fragProbe + (inward * PROBE_INSET);
    fragSkirtProbe = relative + (FACE_NORMALS[normal] * 0.5) + (inward * PROBE_INSET);
    gl_Position = uViewProj * vec4(relative, 1.0);
}
