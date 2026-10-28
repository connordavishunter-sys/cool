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

Recipes live in `src/items.cpp`. The list shows what you can craft first and scrolls with the mouse wheel. Flowers grow
in patches on grass; breaking one gives it to you. Caves below y=10 are filled with lava.

## Building blocks

Planks, logs and all six lights come as slabs, stairs, vertical slabs and sideways stairs. They're oriented as you
place them:

- **Slabs and stairs** go on the bottom half, or upside down when you click the top half of a block's side or the
  underside of a block. Stairs climb away from you.
- **Vertical slabs** sit against the face you click.
- **Sideways stairs** stand upright, with the open corner facing you.
- Placing a slab onto the open half of a matching slab fills the block in.

You walk up slabs and stairs without jumping. Glass panes and stained panes join up with each other and with any
full block next to them. Flowers can only be planted on grass or dirt.

## Lighting modes (Options → Lighting)

- **Classic**: flood-fill sky light and colored RGB block light, baked per vertex with smooth lighting and ambient occlusion.
- **Ray Traced**: `VK_KHR_ray_query` shadow rays toward the sun/moon and the nearest block lights, plus ray traced water reflections.
- **Path Traced**: two-bounce path tracing with next-event estimation and temporally reprojected accumulation.

In both ray traced modes, leaves and glass are alpha tested, so they cast shadows only where their texture is opaque.
Stained glass tints the light that passes through it, so it casts coloured shadows.

Both ray traced modes need a GPU with ray query support; otherwise Classic is used. Lower **Render Scale** in Options for more speed.

## Layout

- `src/world.*`: chunks, terrain generation, threaded lighting/meshing, chunk streaming and RLE save files
- `src/renderer.*`, `src/vk_context.*`: Vulkan setup, chunk meshes, BLAS/TLAS management, frame graph
- `src/blocks.*`: block table, shapes (half-block cell masks), pane connections and collision boxes
- `src/items.*`: inventory, item stacks and crafting recipes
- `src/ui.*`, `src/game.*`: immediate-mode UI, screens, survival rules, saves and options
- `shaders/`: GLSL, compiled to SPIR-V at build time (`world_rt.frag.spv` is the ray query variant)

Dev environment variables: `ORDIS_NOVSYNC=1` (uncapped frame rate), `ORDIS_NO_RT=1` (force non-RT path), `ORDIS_VALIDATION=1` (validation layers).
