# Dynamic shadows in stereo — what has been tried

Status: **fixed** (confirmed in game, 2026-10-27). See "Resolution" below. The
rest of the file is the investigation as it stood before the cause was found;
its conclusions about the decal draw are correct but it missed the silhouette
pass.

## Resolution

The decal was never the problem; its texture was. `func_15184FA4` (called from
`func_15186794`, inside the F9 list) renders the character's silhouette into the
64-wide offscreen image `D_800DF088` through a light camera (`guLookAtF` +
`guPerspectiveF`, raw `G_MTX` `DA380007`), blurs it with texture rectangles, and
the decal samples that image with CPU-computed, eye-independent UVs.

RT64's `popMatrixGroupV1` read the pop's projection bit from word 0 while
`gEXPopMatrixGroup` (and the host) write it in word 1, so the camera hook's pop
never left the projection stack and `434B0001` lingered for the rest of the
display list. The light camera inherited the world tag, got the stereo shear
and eye offset, and each eye rendered a shifted silhouette: a false disparity on
the ground, independent of the decal's geometry (hence the Y tuner's lack of
effect and the shared plane across characters). `[shdwdraw]` filtered on the
decal's model group, so the silhouette draws never appeared in the logs.

Fix (all in `recomp/rt64.patch` for the RT64 parts):
- `tools/rt64/src/gbi/rt64_gbi_extended.cpp`: the pop reads `p1(8, 1)`.
- `tools/rt64/src/hle/rt64_rsp.cpp`: a projection takes the ID group on top of
  the stack when its matrix is loaded (`projectionGroupStack`), not when its
  first vertex is drawn.
- `host/src/stereo.cpp` + `conker.toml`:
  - the camera setup (`func_1515D6D0`) makes the world tag the base of the
    projection stack for the rest of the display list instead of pushing and
    popping it. `func_150D1810` and `func_15019CC8` build world projections from
    the camera outside the setup, and they rely on the tag;
  - `func_15184FA4` (the silhouette pass) runs under an untagged (ID 0) group, so
    the light camera stays mono;
  - `func_1510B7B4` (the world camera restore, which the silhouette pass ends
    with) is wrapped in its own tagged group, so the decal is tagged.

The `[shdwfix]` per-eye shift, the receiver-depth image, the Y tuner and the
`[shdwdraw]`/`[shdwvert]` logs were removed. F9 / `CONKER_NO_SHADOWS` and
`CONKER_SHADOW_MATRIX_LERP` remain. The investigation below refers to tools that
no longer exist (`shadow_y_tune.cmd`, `shadow_drawlog.cmd` and the
`CONKER_SHADOW_*` debug variables).

## The symptom

- The game's dynamic character/prop shadows (Conker, the scarecrow, ...) read as
  floating above the ground in stereo. Mono looks correct.
- The shadow's world position and its stereo plane appear decoupled: the
  temporary Y tuner slid the decal tens of units up (visibly above Conker's
  head) and down (sunk under the floor), and the parallax did not visibly
  change.
- Depth visibility follows the geometry: below the floor the decal is occluded
  and disappears; just above the floor it is visible and already "floats".
- Seen on flat ground and over the pancaked B-button mount.
- With Conker and the scarecrow on screen, both shadows were reported to sit in
  the same stereo plane.

## How the shadows are drawn

- Each shadow is a copy of the ground under the character, clipped anew every
  frame, with the shadow's texture projected onto it. The display list
  (`conker/conker/src/game/game_1B1600.c:1439-1543`) uses G_VTX vertices and
  triangle commands, other-mode `0xEF082C3F / 0x5049D8`.
- Decoded: pixel z-source, `ZMODE_XLU`, z-compare on, z-write off. The decal is
  an ordinary translucent surface depth-tested against the floor, so visibility
  follows the geometry exactly as observed.
- The host wraps that display list in an RT64 matrix group `0x5348xxxx`
  (`host/src/interpolation.cpp:177-204`), which makes the draws identifiable in
  logs and toggleable at runtime (F9 / `CONKER_NO_SHADOWS`).
- Logged draws (`host/build-win/shadow_draw_log.txt`): Perspective, projection
  `434B0001` (the world camera the host tags for stereo), ~2-5 draws per frame
  (72/20/16/8 triangles), vertices with local y = 0, w ≈ 206..364, model-view
  translation (1.0, -88.7, 214.0).
- Stereo is a shear in the projection matrix plus a view-space eye offset
  (`tools/rt64/src/render/rt64_projection_processor.cpp:226-305`). Each vertex's
  NDC x shifts by `-eyeSign * separation * (1 - convergence/z)`, z being that
  vertex's own view depth. Depth is untouched by stereo, so the per-eye depth
  test is unaffected.
- For a decal coplanar with the floor, that math gives the decal exactly the
  floor's parallax. A coplanar decal should be unable to float.

## Attempts to fix it

### 1. Interpolate the shadow matrix (`CONKER_SHADOW_MATRIX_LERP`)

Theory: the decal steps at the game's rate while the camera and the character
are interpolated between game frames, which reads as a float in motion.

Done: the shadow group interpolates its matrix (and its texture coordinates),
`host/src/interpolation.cpp:57-76`; default ON.

Result: the user tested with it off — no change. Closed.

### 2. Per-eye screen offset for shadow decals (`[shdwfix]`)

Theory: the decal is parked slightly above the ground it darkens, and that depth
offset reads as a float in stereo. The host measured the decal's inverse depth
against a receiver-depth readback while building display lists, and turned it
into an extra per-eye screen shift
(`tools/rt64/src/hle/rt64_rsp.cpp:1340-1396`, applied at
`tools/rt64/src/render/rt64_framebuffer_renderer.cpp:1692-1696`).

Measured: diffs of -3e-6 (72-triangle draw) and -1.4e-5 (8-triangle draw) with
separation 0.02 and convergence 120, i.e. shifts of 8e-6..3.4e-5 NDC — about
0.01-0.04 px, far below the ±0.05 clamp
(`host/build-win/shadow_draw_log.txt:32-41`).

Result: inert. The receiver readback is also not trustworthy (it comes from the
rendered depth image, where the decal pass and its neighbours make the sample
ambiguous). Disabled in the tuning runs with `CONKER_SHADOW_STEREO_FIX=0`.
Closed as a fix; the code remains as dead debug code.

### 3. Slide the decals down with a live Y tuner (F5/F6)

Theory: if the game parks the decal above the ground, moving it down would put
it visually on the floor and the float would disappear before it z-clips.

Done: `conker_shadow_y_fix` shifts the shadow groups' world transforms in Y
(`host/src/interpolation.cpp:117-175`, `tools/rt64/src/hle/rt64_rsp.cpp:713-724`).

Observed: the decals could be moved above Conker's head and under the floor (and
clipped when sunk), but the float did not visibly change.

However, the tuner is **buggy**: on a matrix-group change without a new MVP it
copies the current (already shifted) transform
(`tools/rt64/src/hle/rt64_rsp.cpp:686-690`) and then applies the shift again
(`:713-724`), so the applied Y displacement accumulates and is not the plotted
value. The Y-sweep conclusions are void; the tuner needs fixing before it is
used again.

Separately, geometry explains why the plane barely follows Y: at w ≈ 250,
moving the decal tens of units along Y changes its true parallax by roughly a
pixel. Even with perfect math, the stereo plane hardly moves during such a
sweep.

### 4. Checked and ruled out

- **Primitive depth (`G_ZS_PRIM`)**: not used. No `G_SETPRIMDEPTH` in the shadow
  display list; z-source is pixel, so the decal depth-tests with its own
  geometry.
- **`vertexTestZ` extended path**: not used by these draws.
- **A different pass drawing the visible darkening** (rectangle path, projected
  texture, etc.): F9 blanks exactly the func_15186794 display list and the
  shadow disappears; that list contains only G_VTX triangle draws. All 400
  logged shadow-model draws are Perspective (`projtype 1`). The rectangle path
  is not involved.
- **`horizontalMisalignment`**: applied only to low-resolution texture UVs
  (`tools/rt64/src/shaders/RasterPS.hlsl:117-121`), not to position or depth
  sampling, and it is 0 in the triangle path. No color-vs-depth pixel shift.
- **Eye-to-eye depth carry-over**: depth is cleared per eye by the game's own
  fill rectangles (`tools/rt64/src/render/rt64_framebuffer_renderer.cpp:600-611`).
- **A constant per-eye offset on the shadow draw**: every x term was audited
  for IndexedTriangles draws (`rt64_framebuffer_renderer.cpp:1491-1493`,
  `1513-1521`, `1677-1706`, `RasterVS.hlsl:25`). The only shadow-specific
  constant is `[shdwfix]` (item 2, inert). Everything else is identical for both
  eyes and for the floor.
- **The shear reaching the draw**: the shadow's projection carries the tagged
  world-camera ID, its vertices are transformed per eye with the stereo
  view-projection (`tools/rt64/src/shaders/RSPProcessCS.hlsl:53-66`), and the
  upload uses the modified matrices
  (`rt64_projection_processor.cpp:321-322`). The code path says the decals are
  sheared.

## What the captures say

- `host/build-win/sbs_shad_on.png` / `sbs_shad_off.png`: clean side-by-side
  pair at the floaty spot, same pose, F9 between (`shadow_sbs.cmd`). Taken with
  `[shdwfix]` on, but it is inert.
- The old measurement matched the flat shadow-layer difference and gave floor
  +15 px/eye and shadow +12..13 px, a 2-3 px deficit. That method is degenerate
  on a flat blob and is unreliable per pixel.
- Re-measured by matching the shadow's own alpha pattern (high-passed ON−OFF
  layer) against the local floor in the same frame: Conker's blob
  (y 650-950, x 330-680) gives shadow -13 px vs floor -14 px — **+1 px median
  (+2 mean)**, i.e. the shadow reads about 1-3 px nearer than the floor. The
  offset varies with location (roughly 0-4 px across the blob rows). So there
  is a small, real parallax error in this capture, not a gross constant plane.
- `sbs_shadow_y_0.0_two_shadows_different_planes.png`: no OFF frame, faint
  shadows, different framing — the shadows cannot be isolated reliably, so it
  was not used quantitatively. The "two shadows in one plane" observation
  remains unverified by the still.

## Current conclusion

- No renderer-side constant per-eye shift exists on the ordinary shadow draw
  path; the only shadow-specific constant is inert.
- The captured error is about 1-3 px, consistently "nearer" than the floor.
- For that, the decal's depth at the drawn pixels must differ from the
  receiver's (placement/parking), or the shear must not consistently reach that
  draw (which the audited code path says should not happen).
- These two remaining explanations are distinguishable in-game without touching
  the game's geometry: sweep convergence and separation while watching the
  shadow against the ground.

## The open test: convergence / separation sweep

Live stereo-tuning hotkeys were added so this can be done without the settings
overlay (`host/src/stereo.cpp`, built into `host/build-win/ConkerRecomp.exe`):

| Combo | Effect |
|---|---|
| Ctrl + Left / Right | Separation -/+ 0.5% per step |
| Ctrl + Up / Down | Convergence +/- 5 units |
| Alt + Left / Right | Separation -/+ 2.5% per step |
| Alt + Up / Down | Convergence +/- 25 units |

Hold to repeat. Each change prints `[stereo] separation X% convergence Y` on
stderr. The values clamp like the sliders (0-40%, 1-2000). The settings
overlay's sliders do not reflect these changes, and applying any graphics
change from the overlay restores the stored config values. Launching with
`CONKER_STEREO_SEPARATION`/`CONKER_STEREO_CONVERGENCE` set makes the env values
fight the hotkeys on every change; the tuning wrappers do not set them.

Procedure: run `shadow_y_tune.cmd` (Y = 0, `[shdwfix]` off), do not touch F5/F6
(the tuner is buggy), stand at the floaty spot, and sweep convergence.

Interpretation:

- **Shadow slides together with the ground** (their relative offset stays the
  same while the scene's depth moves): the decal is being sheared correctly, and
  the residual error is a placement/depth difference. The fix belongs in the
  decal's depth or per-eye position, not in the stereo transform.
- **Shadow stays pinned while the ground slides**: the shadow is not getting the
  shear, or gets a constant one. That is a renderer-side bug in this draw path
  and matches the "one shared plane" report.
- **Separation sweep refines it**: a shear/depth mismatch grows with separation;
  a fixed screen-space offset would not.

## Key code locations

- Game shadow display list: `conker/conker/src/game/game_1B1600.c:1439-1543`.
- Shadow group hooks, matrix interpolation, Y tuner, F9:
  `host/src/interpolation.cpp` (57-76, 117-175, 177-204).
- Stereo hotkeys and world-camera tagging: `host/src/stereo.cpp`.
- Stereo shear, eye offset, per-eye upload:
  `tools/rt64/src/render/rt64_projection_processor.cpp:221-322`.
- Per-vertex transform: `tools/rt64/src/shaders/RSPProcessCS.hlsl:50-66`.
- Screen scale/offset and `[shdwfix]`:
  `tools/rt64/src/render/rt64_framebuffer_renderer.cpp:1491-1521, 1677-1706`;
  `tools/rt64/src/shaders/RasterVS.hlsl:24-25`.
- Decal depth check and texture UV snap:
  `tools/rt64/src/shaders/RasterPS.hlsl:91-121`.
- Shadow receiver depth sampling: `tools/rt64/src/hle/rt64_rsp.cpp:1340-1396`.
- Y tuner accumulation: `tools/rt64/src/hle/rt64_rsp.cpp:686-724`.

## Artifacts

- `host/build-win/shadow_draw_log.txt` — 400 `[shdwdraw]` and `[shdwvert]`
  lines plus `[shdwfix]` shifts, at separation 0.02 / convergence 120.
- `host/build-win/shadow_log.txt` — auto-convergence and projection logs.
- `host/build-win/shadow_depth_log.txt` — depth-dump run log.
- `host/build-win/sbs_shad_on.png`, `sbs_shad_off.png` — clean SBS pair with
  and without the shadows.
- `host/build-win/sbs_shadow_y_0.0_two_shadows_different_planes.png` — the
  two-shadow spot (no OFF pair; not usable for measurement).
- `host/build-win/shadow_on.png`, `shadow_off.png`, `shadow_compare.png`,
  `shadow_pair.png` — earlier captures.
- Wrappers: `shadow_sbs.cmd`, `shadow_y_tune.cmd`, `shadow_drawlog.cmd`,
  `shadow_depth.cmd`, `run_noshadows.cmd`.
- Measurement scripts from this investigation (outside the repo):
  `%TEMP%\opencode\` (`shad_measure2.py`,
  `shad_pattern.py`, `shad_overlay2.py`, `shad_two2.py`, ...).
