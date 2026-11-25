// Stereoscopy: projection tagging, called from hooks in conker.toml.
//
// RT64 applies its stereo shift to the projections it can identify as "the camera's".
// The game tells it which those are with an RT64 matrix group tagged with the stereo
// world matrix ID; a projection carries the group that is on top of RT64's stack when
// the projection matrix is loaded. RT64 only gives projections carrying that ID the
// stereo shift when any projection in the frame has it; menus, FMV and other untagged
// perspective projections then stay flat. Frames with no tagged projection at all fall
// back to shifting every perspective one, so a game path that doesn't run the camera
// setup keeps working as before.
//
// The camera setup (func_1515D6D0) makes the tag the base of the projection stack for
// the rest of the display list rather than pushing and popping it around its own load:
// the game builds other world projections from the camera outside it (func_150D1810
// and func_15019CC8 multiply the camera's projection into their own), and those need
// the stereo shift too. Two passes are bracketed explicitly:
// - the shadow's silhouette texture (func_15184FA4) is rendered through a light camera
//   that must stay out of stereo, so it runs under an untagged group;
// - the camera restore that pass ends with (func_1510B7B4, also used elsewhere) loads
//   the world camera again, so it is tagged explicitly, inside or outside a bracket.
//
// The ID must match conkerStereoWorldMatrixId in tools/rt64
// (src/render/rt64_projection_processor.cpp).
//
// Live stereo knobs: Ctrl/Alt + arrows slide the separation and the convergence, and
// Ctrl/Alt + Page Up/Page Down the HUD depth, without the settings overlay (see
// poll_stereo_hotkeys). The changes go through
// ultramodern's graphics config, the same path the overlay's sliders use, so the
// renderer picks them up on the next frame. The sliders themselves do not show
// these values: opening the overlay and touching any of them re-applies the
// config's stored values and undoes the hotkeys.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <SDL.h>

#include "recomp.h"
#include "ultramodern/config.hpp"

namespace {
    // RT64's extended GBI (tools/rt64/include/rt64_extended_gbi.h) for F3DEX2,
    // whose no-op (0xE0) carries RT64's hooks.
    constexpr uint32_t rt64_hook_opcode = 0xE0;
    constexpr uint32_t rt64_hook_magic = 0x525464;
    constexpr uint32_t rt64_hook_op_enable = 0x1;
    constexpr uint32_t rt64_extended_opcode = 0x64;
    constexpr uint32_t g_ex_matrixgroup_v1 = 0x00000C;
    constexpr uint32_t g_ex_popmatrixgroup_v1 = 0x00000D;
    // G_EX_SETSTEREOSCRIPTED_V1: whether the frame is a scripted scene (the view is
    // built from a camera that isn't the one being played), where the game's sprite
    // depths come from the parked world. RT64's rectangles then use the scene's focus
    // depth (the auto-convergence loop's) instead of their own.
    constexpr uint32_t g_ex_setstereoscripted_v1 = 0x000036; // 0x34 and 0x35 are the blur and grey framebuffer (widescreen.cpp, cutscene_aspect.cpp)
    constexpr uint32_t conker_stereo_world_id = 0x434B0001;
    // The camera being played (D_800DBFF0).
    constexpr uint32_t current_play_camera = 0x800DBFF0;

    void put_command(uint8_t* rdram, gpr& dl, uint32_t w0, uint32_t w1) {
        MEM_W(0, dl) = (int32_t)w0;
        MEM_W(4, dl) = (int32_t)w1;
        dl += 8;
    }

    // gEXMatrixGroup's interpolation for a camera: simple (not decomposed) interpolation of
    // its position, rotation, scale, skew and perspective (bits 3, 5, 7, 9 and 11 set to
    // G_EX_COMPONENT_INTERPOLATE), as RT64 interpolates a camera it has no group for. Left
    // at zero these are G_EX_COMPONENT_SKIP, and RT64 then shows the camera at the game's
    // own frame rate while everything else is interpolated.
    constexpr uint32_t camera_interpolation_params = (1u << 3) | (1u << 5) | (1u << 7) | (1u << 9) | (1u << 11);

    // gEXMatrixGroup for the projection stack (projection = bit 1, push = bit 0), with
    // RT64's extended GBI enabled first (RT64 turns it off at the start of every
    // display list). Without push, the group replaces the one on top. interpolation is
    // the group's component bits (camera_interpolation_params for the world camera).
    void put_projection_group(uint8_t* rdram, gpr& dl, uint32_t id, bool push, uint32_t interpolation) {
        put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_matrixgroup_v1, id);
        put_command(rdram, dl, 0x2 | (push ? 0x1 : 0x0) | interpolation, 0);
    }

    // gEXPopMatrixGroup: count 1, projection (bit 8 of the second word).
    void put_projection_pop(uint8_t* rdram, gpr& dl) {
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_popmatrixgroup_v1, 1 | (1 << 8));
    }

    // Stereo knobs without the settings overlay: Ctrl + arrows is the fine step
    // (0.5% separation, 5 units convergence), Alt the coarse one (2.5%, 25); Page
    // Up/Page Down move the HUD depth (5% / 25% of the separation, up brings it out).
    // The current values print as [stereo]. Polled from the camera setup, which runs
    // every gameplay frame; a held combo repeats after a short delay.
    void poll_stereo_hotkeys() {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        if (keys == nullptr) {
            return;
        }

        const bool ctrl = (keys[SDL_SCANCODE_LCTRL] != 0) || (keys[SDL_SCANCODE_RCTRL] != 0);
        const bool alt = (keys[SDL_SCANCODE_LALT] != 0) || (keys[SDL_SCANCODE_RALT] != 0);
        constexpr int keyCount = 6;
        const bool down[keyCount] = {
            keys[SDL_SCANCODE_LEFT] != 0,
            keys[SDL_SCANCODE_RIGHT] != 0,
            keys[SDL_SCANCODE_UP] != 0,
            keys[SDL_SCANCODE_DOWN] != 0,
            keys[SDL_SCANCODE_PAGEUP] != 0,
            keys[SDL_SCANCODE_PAGEDOWN] != 0
        };

        static bool prevDown[keyCount] = {};
        static Uint32 nextRepeat[keyCount] = {};

        const Uint32 now = SDL_GetTicks();
        bool apply[keyCount] = {};
        bool any = false;
        for (int i = 0; i < keyCount; i++) {
            // The subtraction wraps, so this also holds across SDL_GetTicks' rollover.
            if (down[i] && (ctrl || alt) && (!prevDown[i] || (int32_t(now - nextRepeat[i]) >= 0))) {
                nextRepeat[i] = now + (prevDown[i] ? 80 : 400);
                apply[i] = true;
                any = true;
            }

            prevDown[i] = down[i];
        }

        if (!any) {
            return;
        }

        ultramodern::renderer::GraphicsConfig config = ultramodern::renderer::get_graphics_config();
        const double separationStep = alt ? 0.025 : 0.005;
        const double convergenceStep = alt ? 25.0 : 5.0;
        const double hudDepthStep = alt ? 25.0 : 5.0;
        if (apply[0]) {
            config.stereo_separation_option -= separationStep;
        }

        if (apply[1]) {
            config.stereo_separation_option += separationStep;
        }

        if (apply[2]) {
            config.stereo_convergence_option += convergenceStep;
        }

        if (apply[3]) {
            config.stereo_convergence_option -= convergenceStep;
        }

        if (apply[4]) {
            config.stereo_hud_depth_option += hudDepthStep;
        }

        if (apply[5]) {
            config.stereo_hud_depth_option -= hudDepthStep;
        }

        config.stereo_separation_option = std::clamp(config.stereo_separation_option, 0.0, 0.40);
        config.stereo_convergence_option = std::clamp(config.stereo_convergence_option, 1.0, 2000.0);
        config.stereo_hud_depth_option = std::clamp(config.stereo_hud_depth_option, -100.0, 100.0);
        ultramodern::renderer::set_graphics_config(config);
        std::fprintf(stderr, "[stereo] separation %.1f%% convergence %.0f hud %.0f%%\n",
            config.stereo_separation_option * 100.0, config.stereo_convergence_option, config.stereo_hud_depth_option);
    }
}

// Whether the game is in the opening's cutscene sequence (D_800BE9F0 == 0x21), where
// the played camera and the world's objects sit parked while a camera of the scene's
// own draws the picture, so the game's sprite depths come from that parked world.
// Written by the frame-start hook in host/src/render_fixes.cpp.
bool conker_stereo_play_camera_frozen = false;

// func_1515D6D0, after its stack frame: $a0 is the display list it writes the camera
// setup (including the projection matrix) into.
extern "C" void conker_stereo_world_begin(uint8_t* rdram, recomp_context* ctx) {
    poll_stereo_hotkeys();
    gpr dl = ctx->r4;
    // Not pushed: the tag replaces the base of the projection stack, so the projections
    // the game builds from the camera later in the display list carry it too.
    put_projection_group(rdram, dl, conker_stereo_world_id, false, camera_interpolation_params);

    // The opening's cutscene sequence renders through a camera of its own while the
    // played one stays parked, and the game's sprites keep the depths of that parked
    // world. Tell RT64, whose rectangles then use the auto-convergence focus depth
    // instead of those.
    const bool scripted = conker_stereo_play_camera_frozen;
    put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_setstereoscripted_v1, scripted ? 1u : 0u);
    ctx->r4 = dl;

    // TEMP-DEBUG: CONKER_STEREO_SCRIPTED_LOG prints the flag.
    static const bool scriptedLog = SDL_getenv("CONKER_STEREO_SCRIPTED_LOG") != nullptr;
    static int scriptedLogCount = 0;
    if (scriptedLog && ((scriptedLogCount++ % 60) == 0)) {
        std::fprintf(stderr, "[host] scripted %d\n", scripted ? 1 : 0);
    }
}

// func_1510B7B4 reloads the world camera's view and projection from the camera record
// (after the shadow's silhouette pass and other offscreen passes). Before it writes
// anything: $a0 is its display list.
extern "C" void conker_stereo_restore_begin(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r4;
    put_projection_group(rdram, dl, conker_stereo_world_id, true, camera_interpolation_params);
    ctx->r4 = dl;
}

// At its return: $v0 is the end of what it wrote.
extern "C" void conker_stereo_restore_end(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r2;
    put_projection_pop(rdram, dl);
    ctx->r2 = dl;
}

// func_15184FA4 renders the shadow's silhouette texture through a light camera before
// the decal samples it. That pass must stay out of stereo: if the eye shift reaches it,
// each eye's texture content moves, and the decal (whose texture coordinates are the
// same for both eyes) shows the difference as a false disparity on the ground. Wrap
// the whole pass in an untagged projection group (ID 0, G_EX_ID_IGNORE); the camera
// restore it ends with tags itself.
extern "C" void conker_shadow_image_begin(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r4;
    put_projection_group(rdram, dl, 0, true, 0);
    ctx->r4 = dl;
}

// At its return: $v0 is the end of what it wrote.
extern "C" void conker_shadow_image_end(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r2;
    put_projection_pop(rdram, dl);
    ctx->r2 = dl;
}
