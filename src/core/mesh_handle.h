#ifndef MESH_HANDLE_H
#define MESH_HANDLE_H

#include <stdint.h>

// Opaque reference to a GPU mesh owned by the render backend. Logical structs
// (e.g. Chunk) store handles instead of renderer-specific mesh types, keeping
// them renderer-agnostic. The backend maps a handle to its concrete mesh.
typedef uint32_t MeshHandle;

#define MESH_HANDLE_INVALID ((MeshHandle)0xFFFFFFFFU)

#endif
