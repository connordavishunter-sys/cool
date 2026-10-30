# Ordis

A voxel sandbox with survival and creative modes, written in C++20 with SDL2 and Vulkan 1.3. It has an infinite world, 16×16×256 chunks,
and three lighting modes, two of them hardware ray traced.

## Building

Requirements: CMake, a C++20 compiler, `glslc`, GLM, the Vulkan SDK/loader, SDL2, SDL2_image, SDL2_mixer and SDL2_ttf.

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

```bash
./build/ordis
```

Saves go to `build/saves/<world>/`, options to `build/options.txt`.

DLSS is optional. When NVIDIA's [DLSS SDK](https://github.com/NVIDIA/DLSS) is in `third_party/DLSS` (only `include/`,
`lib/Linux_x86_64/libnvsdk_ngx.a` and `lib/Linux_x86_64/rel/libnvidia-ngx-dlss*.so.*` are needed), CMake builds it in and copies the
DLSS libraries (upscaling and frame generation) next to the binary; without it the DLSS options are hidden. The SDK is under NVIDIA's license (`third_party/DLSS/LICENSE.txt`).

## Controls

| Key | Action |
| --- | --- |
| WASD / mouse | Move / look |
| Space | Jump; in creative, double-tap to toggle flying and hold to fly up |
| Left Shift | Fly down |
| Left Ctrl | Sprint |
| Left click / right click | Break (hold in survival) / place block or eat food |
| Middle click | Pick the targeted block (survival: select it if it's in the hotbar) |
| 1–9, mouse wheel | Select hotbar slot |
| E | Inventory: every block in creative; storage and crafting in survival |
| F3 | Debug overlay |
| F4 | Cycle lighting mode |
| T or / | Chat and commands (Up/Down recalls earlier messages) |
| F11 | Fullscreen |
| Esc | Pause menu |

## Game modes

Picked when creating a world. Worlds made before survival existed open in creative.

- **Creative**: fly, every block available, instant breaking.
- **Survival**: blocks take time to break and go into your inventory (36 slots, stacks of 64). You have 10 hearts and
  10 hunger. Hunger drains as you sprint, jump, swim and break blocks, and you heal while it's at 9 or more. At 0 you
  starve down to half a heart. Falling more than 3 blocks hurts, and so does lava. Breaking leaves sometimes drops
  food; hold it and right click to eat. When you die you respawn at the world spawn and keep your inventory.

In the survival inventory, click a slot to pick up a stack and click again to drop, merge or swap it. Right click
splits a stack or drops a single item, and shift-click moves a stack between the hotbar and storage. Crafting is a
list: click a recipe to make one.

| Makes | From |
| --- | --- |
| 4 Planks | 1 Log |
| Glass | Sand |
| Red / Orange / Yellow / Green / Purple Stained Glass | Glass + Dirt / Log / Sand / Leaves / Stone |
| White Light | 2 Glass + Planks |
| Red / Orange / Yellow / Purple Light | White Light + matching Stained Glass |
| Blue Light | White Light + Stone |
| 16 Glass Pane / Stained Pane | 6 Glass / matching Stained Glass |
| 6 Slabs | 3 Planks, Log or Light |
| 4 Stairs | 6 Planks, Log or Light |
| Vertical Slab | Slab of the same material |
| Sideways Stairs | Stairs of the same material |
| 8 Panels | 1 Stone, Planks or Log |
| Stone / Planks / Log | 8 matching Panels |

Recipes live in `src/items.cpp`. The list shows what you can craft first and scrolls with the mouse wheel. Flowers grow
in patches on grass; breaking one gives it to you. Caves below y=10 are filled with lava.

## Building blocks

Planks, logs, stone and all six lights come as slabs, stairs, vertical slabs and sideways stairs. They're oriented as you
place them:

- **Slabs and stairs** go on the bottom half, or upside down when you click the top half of a block's side or the
  underside of a block. Stairs climb away from you.
- **Vertical slabs** sit against the face you click.
- **Sideways stairs** stand upright, with the open corner facing you.
- Placing a slab onto the open half of a matching slab fills the block in.
- **Panels** (stone, planks and log) are 1/8 of a block thick, on the floor of a block or hanging from its ceiling
  when you click the top half of a side or an underside. Clicking a panel's open face, or placing into its cell, with
  the same panel makes it an eighth thicker; at eight eighths it becomes the full block. Breaking one gives back every
  eighth.
- **Stairs connect** like the original game. A perpendicular stair behind one turns it into an outer corner, and one in
  front makes an inner corner, unless that would break a straight run. Collision and the selection box follow the corner.

You walk up slabs and stairs without jumping. Glass panes and stained panes join up with each other and with any
full block next to them. Flowers can only be planted on grass or dirt.

## Lighting modes (Options → Lighting)

- **Classic**: flood-fill sky light and colored RGB block light, baked per vertex with smooth lighting and ambient occlusion.
- **Ray Traced**: `VK_KHR_ray_query` shadow rays toward the sun/moon and the nearest block lights, plus ray traced water reflections.
- **Path Traced**: two-bounce path tracing with next-event estimation and temporally reprojected accumulation.

Overlapping coloured lights add up, so a red and a blue light make purple where they meet. In Classic each light colour
floods separately and the colours are summed in linear light; the ray traced modes add the lights directly, with a
gentle falloff so neighbouring lights overlap.

In both ray traced modes, leaves and glass are alpha tested, so they cast shadows only where their texture is opaque.
Stained glass tints every ray that passes through it (shadow, bounce and reflection rays), so it casts coloured shadows
and tints what is seen through reflections. In Classic it tints block light, but not daylight, which has only a single
brightness channel.

Both ray traced modes need a GPU with ray query support; otherwise Classic is used. Lower **Render Scale** in Options for more speed.

**Options → Ray Tracing Settings...** tunes both modes: how many block lights get shadow rays, sun shadow softness, sky
ambient and water reflections for Ray Traced; bounces, samples per pixel, accumulation length and sun softness for Path
Traced; and block light strength for both. Changing a setting restarts path traced accumulation.

## Anti-aliasing (Options → Anti-aliasing)

- **FXAA**: post-process edge smoothing, the cheapest option.
- **TAA**: jitters the camera by a sub-pixel offset each frame and blends the frames using motion vectors reprojected from the
  depth buffer, clamping history to the current pixel's neighbourhood so moving edges don't ghost.
- **DLAA / DLSS Quality / Balanced / Performance / Ultra Performance**: NVIDIA DLSS Super Resolution on RTX GPUs, fed the same
  jitter and motion vectors. DLSS picks the render resolution (100%, 67%, 58%, 50%, 33%), so Render Scale is ignored. In Path
  Traced mode DLSS takes the raw frames and replaces the accumulation pass.

The F3 overlay shows the anti-aliasing mode and the render and output resolutions.

## Frame generation (Options → Frame Generation)

DLSS Frame Generation (RTX 40 series and newer) inserts an interpolated frame between every two rendered frames, using the
depth and motion vectors from the world pass. The UI is drawn over a HUD-less copy of the scene, so it never warps. A pacer
thread presents the generated frame as soon as the GPU finishes and the real one half a frame later. It works with any
anti-aliasing mode; the F3 overlay shows both the rendered and the presented frame rate. Generating a frame costs about 3 ms
on an RTX 4070 Laptop GPU at 1128x744, so it helps most in heavy scenes like Path Traced.

## Particles

Mining and breaking blocks throws off small chips of the block's texture that fall and land on the terrain. They're drawn
through the world pipeline, so every lighting mode lights them.

## Water and lava

The surfaces of water and lava roll with gentle waves. The mesher flags the top vertices of each liquid surface, and the
vertex shader lowers them along a few sine waves in world space; lava moves slower and heavier. The fragment shader tilts
the normal to match, so reflections and Fresnel ripple in the ray traced modes. Flat-lit water in Classic gets slope shading,
and lava darkens in its troughs.

## Clouds

A layer of blocky 12×4×12 clouds floats at y=192 and drifts east. They're drawn as translucent boxes in every lighting
mode, extend to twice the render distance (up to 384 blocks) and fade out by horizontal distance.

## Commands and game rules

Press T to chat, or / to start a command. Coordinates accept `~` for relative positions, and item and block names are their
display names in lower case with underscores (`stone_stairs`, `red_stained_glass`, `food`).

| Command | Effect |
| --- | --- |
| `/help` | List commands |
| `/gamemode <survival\|creative>` | Switch game mode |
| `/time set <day\|noon\|night\|midnight\|ticks>`, `/time add <ticks>`, `/time query` | Time of day, 24000 ticks per day |
| `/tp <x> <y> <z>` | Teleport |
| `/give <item> [count]` | Add items to your inventory |
| `/setblock <x> <y> <z> <block>` | Place one block |
| `/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>` | Fill a box, up to 32768 blocks |
| `/gamerule [rule] [true\|false]` | List, show or set a game rule |
| `/spawnpoint`, `/kill`, `/clear`, `/seed` | Set spawn here, die, empty inventory, show the seed |

Game rules are saved per world in `level.dat`:

| Rule | Default | Effect |
| --- | --- | --- |
| `doDaylightCycle` | true | Time of day advances |
| `keepInventory` | true | Keep your items when you die |
| `fallDamage` | true | Falling hurts |
| `fireDamage` | true | Lava hurts |
| `naturalRegeneration` | true | Health regenerates when well fed |
| `doTileDrops` | true | Broken blocks drop items |

## Layout

- `src/world.*`: chunks, terrain generation, threaded lighting/meshing, chunk streaming and RLE save files
- `src/renderer.*`, `src/vk_context.*`: Vulkan setup, chunk meshes, BLAS/TLAS management, frame graph
- `src/dlss.*`: DLSS upscaling and frame generation through NVIDIA NGX (stubs when built without the SDK)
- `src/particles.*`: block breaking particles
- `src/clouds.*`: the cloud layer
- `src/commands.cpp`: chat box, commands and game rules
- `src/blocks.*`: block table, shapes (half-block cell masks), panels, pane connections and collision boxes
- `src/items.*`: inventory, item stacks and crafting recipes
- `src/ui.*`, `src/game.*`: immediate-mode UI, screens, survival rules, saves and options
- `shaders/`: GLSL, compiled to SPIR-V at build time (`world_rt.frag.spv` is the ray query variant)

Dev environment variables: `ORDIS_NOVSYNC=1` (uncapped frame rate), `ORDIS_NO_RT=1` (force non-RT path), `ORDIS_VALIDATION=1` (validation layers).
