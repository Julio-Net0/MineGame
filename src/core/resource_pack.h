#ifndef CORE_RESOURCE_PACK_H
#define CORE_RESOURCE_PACK_H

// Renderer-free resolution of Minecraft resource paths. Every `.zip` pack in
// the external directory is consulted in filename order before the base asset
// directory, so any consumer (block textures, GUI sprites, fonts) gets resource
// pack overrides through the same rules. Archives are opened once at init and
// stay open until shutdown, so each Central Directory is parsed once per run.

// Each category maps a relative path onto both kinds of source:
//   TEXTURE   pack: assets/minecraft/textures/<rel>  base: <root>/<rel>
//   FONT_DEF  pack: assets/minecraft/font/<rel>      base: <root>/font/<rel>
// where <root> is assets/textures (release layout) or textures (source tree).
typedef enum {
  RESOURCE_TEXTURE,
  RESOURCE_FONT_DEF
} ResourceCategory;

// Source identifiers reported by reads: a pack index (0..count-1), or one of
// these.
enum {
  RESOURCE_SOURCE_BASE = -1,
  RESOURCE_SOURCE_NONE = -2
};

void ResourcePacksInit(void);
void ResourcePacksShutdown(void);

int ResourcePackCount(void);

// Read RelPath from the first source that has it, inflated. Returns a heap
// buffer (caller frees with free()) followed by one zero byte not counted in
// OutSize, so text resources can be parsed in place. OutSource may be null.
// Returns null, with OutSource set to RESOURCE_SOURCE_NONE, when no source has
// it.
unsigned char *ResourceRead(ResourceCategory Category, const char *RelPath,
                            int *OutSize, int *OutSource);

// Like ResourceRead, but consults only Source. Used to take a companion file
// (an .mcmeta) from the same pack as the image it describes.
unsigned char *ResourceReadFrom(ResourceCategory Category, const char *RelPath,
                                int Source, int *OutSize);

#endif
