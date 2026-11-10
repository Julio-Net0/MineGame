#ifndef WORLD_BLOCK_ID_H
#define WORLD_BLOCK_ID_H

#include <stdint.h>

// Registry id of a block type. Kept in its own header so chunk storage, the wire
// protocol and player state can name it without pulling in the block registry.
typedef uint16_t BlockID;

#define BLOCK_ID_AIR ((BlockID)0)

#endif
