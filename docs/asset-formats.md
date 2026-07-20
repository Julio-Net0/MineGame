# Asset Formats

Assets are plain JSON, loaded at startup and compiled into packed in-memory structures. Nothing here is compiled into the binary — editing these files and restarting is enough to change the world.

> Only the biome format and the block properties it depends on are documented so far. The full block and prefab schemas are not yet written up.

## Biomes

A biome is a region of a **climate space**, not a region of the map. Generation samples the climate at a position, finds which biome's declared ranges contain that point, and uses that biome's palette.

### Climate axes

| Axis | Source | Range |
|---|---|---|
| `temperature` | 2D fBm noise over (X, Z) | `[0, 1]` |
| `humidity` | 2D fBm noise over (X, Z), decorrelated from temperature | `[0, 1]` |
| `depth` | **Derived**: `TerrainHeight - Y` | blocks |

`depth` is what makes biomes three-dimensional. It costs no noise sample — it is simply how far below that column's surface a position sits. Negative above the terrain, `0` at the surface, positive below. Two positions sharing X and Z can therefore resolve to different biomes purely by height, which is how a cave biome coexists under a forest.

Because the noise is fBm without normalization, climate values cluster toward the middle: in practice `temperature` and `humidity` land roughly in `[0.20, 0.84]`, with about 77% of samples between `0.4` and `0.6`. Ranges near the extremes will almost never match. Tune against `/biome`, not against intuition.

### `assets/biomes/*.json`

One file per biome. Unknown fields are ignored; a malformed file is reported and skipped rather than aborting the load.

```json
{
  "id": 1,
  "name": "Forest",
  "temperature": [0.0, 1.0],
  "humidity": [0.52, 1.0],
  "depth": [-1000.0, 1000000.0],
  "priority": 1,
  "surfaceBlock": "Grass",
  "subsurfaceBlock": "Dirt",
  "underwaterBlock": "Sand",
  "subsurfaceDepth": 3,
  "grassTint": [59, 129, 39],
  "foliageTint": [42, 110, 28]
}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Registry slot, `0`–`63`. Must be unique. **Id `0` is the fallback** used when a climate point matches no biome, so it should have wide ranges. |
| `name` | no | Shown by `/biome`. |
| `temperature` | no | `[min, max]`, inclusive. Omitted means unconstrained. |
| `humidity` | no | `[min, max]`, inclusive. Omitted means unconstrained. |
| `depth` | no | `[min, max]` in blocks below the surface. Omitted means unconstrained. |
| `priority` | no | Highest wins when a point falls inside several biomes. Default `0`. |
| `surfaceBlock` | **yes** | Block **name**, resolved to an id at load. Placed at the terrain surface. |
| `subsurfaceBlock` | **yes** | Placed for `subsurfaceDepth` blocks below the surface. |
| `underwaterBlock` | **yes** | Replaces `surfaceBlock` where the surface is at or below sea level. |
| `subsurfaceDepth` | no | How many blocks of `subsurfaceBlock` sit under the surface. Default `3`. |
| `grassTint` | no | `[R, G, B]` for blocks declaring `"tint": "grass"`. Default white (no tint). |
| `foliageTint` | no | `[R, G, B]` for blocks declaring `"tint": "foliage"`. Default white. |

Block fields take **names**, not numeric ids, and are resolved against the block registry when the biome loads. A name that matches no loaded block is an error and the biome is skipped — a biome with a wrong palette is a bug, not something to paper over.

Air, water, and stone are **not** part of any palette: they are the biome-independent base that generation places above the terrain, up to sea level, and below the subsurface layer.

### Overlap and fallback

Ranges may overlap freely; `priority` decides. This is how a broad default and a narrow special case coexist:

```
plains  temperature [0.00, 1.00]  humidity [0.00, 0.52]  priority 0
forest  temperature [0.00, 1.00]  humidity [0.52, 1.00]  priority 1
desert  temperature [0.56, 1.00]  humidity [0.00, 0.44]  priority 2   <- wins inside plains
cave    depth       [8, 1000000]                          priority 10  <- wins below any surface
```

A point matching nothing falls back to biome `0`.

### `assets/biome_params.json`

Shared climate-noise tuning. A missing or malformed file leaves the built-in defaults in place.

```json
{
  "scale": 0.008,
  "octaves": 3,
  "lacunarity": 2.0,
  "gain": 0.5
}
```

| Field | Meaning |
|---|---|
| `scale` | Noise frequency. Lower means larger biomes. `0.008` gives features roughly 125 blocks across. |
| `octaves` | fBm octave count. More means noisier borders. |
| `lacunarity` | Frequency multiplier per octave. |
| `gain` | Amplitude multiplier per octave. |

### Resolution

Biomes are stored **one id per 4×4×4 block cell**, sampled at the cell's centre block — 64 bytes per chunk instead of 4096. Climate is a low-frequency field, so a cell's blocks share a biome anyway. The visible consequence is that a surface palette changes in 4-block steps at a border; because borders follow irregular noise contours, this does not read as a grid.

The biome map is **derived, never saved**. It is recomputed from the seed whenever a chunk is generated or read back from disk, so changing any file here changes the world without touching the save format. Existing terrain in saved chunks keeps its blocks and will not match a retuned palette until those chunks are regenerated.

## Block tint

Blocks opt into biome colouring with an optional `"tint"` property in `assets/blocks/*.json`:

```json
{
  "id": 1,
  "name": "Grass",
  "texTop": 0,
  "texBottom": 2,
  "texSide": 3,
  "tint": "grass",
  "isTransparent": false
}
```

| Value | Palette used | Faces affected |
|---|---|---|
| `"grass"` | the biome's `grassTint` | top and sides (not the bottom) |
| `"foliage"` | the biome's `foliageTint` | all six faces |
| `"none"`, absent, or unrecognized | none | none — the texture keeps its own colours |

A tinted block's texture should be authored **grayscale** where it is meant to take colour, since the tint is a multiply: grey × green reads as green, but brown × green reads as mud.

Tint travels in the RGB of the per-vertex colour channel, with ambient occlusion in that channel's alpha. The two are kept separate rather than pre-multiplied, because the tint applies per texel and AO per face. No extra vertex attribute is involved.

## Side overlay

A block may declare `"texSideOverlay"`, naming a second atlas tile composited over its **side** faces:

```json
{
  "id": 1,
  "name": "Grass",
  "texTop": 0,
  "texBottom": 2,
  "texSide": 3,
  "texSideOverlay": 180,
  "tint": "grass",
  "isTransparent": false
}
```

Where a face names an overlay, the shader composites it over the base by the overlay's alpha and tints **only the overlay** — the base texture is never tinted. Where a face names none, the tint applies to the whole texel.

That distinction is the point. The grass side tile mixes a grass fringe with dirt, and the dirt carries small grey debris specks. No colour test can separate the fringe from the debris; both are grey. The overlay's alpha says which pixels are grass, so the fringe takes the biome colour and the debris stays as drawn.

Blocks whose whole texture is tintable — grass top, leaves — declare no overlay. Untinted blocks declare none either, and their white tint makes the multiply a no-op however grey their texture is (stone included).

The overlay layer index rides the previously unused second component of the `texcoords2` vertex attribute, so it costs no new attribute — only one extra texture fetch on the faces that have one.

Inside a biome, a tinted block renders the palette's declared colour exactly. Across a border the tint is interpolated over about four blocks. Only the transition band splits merged faces; measured cost is about **+0.7% vertices**.

## Debugging

`/biome` reports the resolved biome at the player's position along with the raw `temperature`, `humidity`, and `depth` that produced it, plus the surface height of that column. It is the intended way to tune ranges: read the real values at a spot, then set ranges that actually contain them.
