#ifndef WORLD_SAVE_H
#define WORLD_SAVE_H

#include <stdint.h>

void InitWorldSave(void);
void CloseWorldSave(void);
uint64_t GetWorldSeed(void);

#endif // WORLD_SAVE_H
