// A skip button, called from the frame-start hook in host/src/render_fixes.cpp.
//
// The game has its own cutscene skip (func_1501E05C): START in the opening's cutscene
// sequence (mode 0x21) and L elsewhere (gated on per-mode section masks). The opening's
// condition also needs osResetType (D_8000030C) to be 1, and the port boots cold with 0,
// so the opening could not be skipped at all. This sets the flag and presses the game's
// own skip button: on the skip button's press edge, the next few frames of port 0 input
// carry START (in the opening) or L (elsewhere; frontend.cpp ORs conker_skip_pad_buttons
// into the pad). The engine then starts its own 0x1E-frame fade, which fast-forwards the
// section's script (game_50D80.c scales its event times by the fade timer), ends the
// section (func_1501DF04 / func_1501DE18 / func_1501E81C) and loads the next one.
//
// (Forcing the section's elapsed frames to its duration was tried and dead-ends:
// sections are script-driven, and only the script's own end queues the next section
// (D_800C35C2), so the game just stops with no section playing. The fade is what
// fast-forwards the script, so it must run.)
//
// The input is raw, so it works without bindings: SDL's keyboard state for Backspace,
// and recompinput's get_input_digital (which reads the SDL controller's button) for the
// controller's right shoulder, unbound by default. Neither is bound to anything else
// (Esc, the settings overlay's key, is deliberately left alone); while a menu captures
// input the press is ignored, so deleting text in a menu's text field never skips.

#include <cstdint>
#include <cstdio>

#include "recomp.h"

#if defined(CONKER_RT64)
#include <SDL.h>

#include "recompinput/input_state.h"
#include "recompinput/input_types.h"
#endif

// The N64 buttons the skip button presses, for frontend.cpp's get_port_input to OR into
// port 0's input. START (0x1000) in the opening, L (0x0020) elsewhere; zero when idle.
// Written once a frame by conker_skip_update.
uint16_t conker_skip_pad_buttons = 0;

namespace {
    // D_8000030C is osResetType and D_800BE9F0 the game mode (0x21 marks the opening's
    // cutscene sequence: see render_fixes.cpp).
    constexpr uint32_t os_reset_type = 0x8000030C;
    constexpr uint32_t game_mode_pointer = 0x800BE9F0;
    constexpr uint32_t game_mode_intro_movie = 0x21;
    constexpr uint16_t n64_start = 0x1000;
    constexpr uint16_t n64_l = 0x0020;
    // How long the game's skip button stays pressed after the edge. The game reads it
    // on its next input poll; stopping after a few frames keeps a held button from
    // running through section after section as each fade finishes.
    constexpr int press_frames = 10;
}

extern "C" void conker_skip_update(uint8_t* rdram) {
    // func_1501E05C's opening skip wants osResetType == 1; the port starts cold with 0.
    static bool reset_type_set = false;
    if (!reset_type_set) {
        MEM_W(0, (gpr)(int32_t)os_reset_type) = 1;
        reset_type_set = true;
    }

#if defined(CONKER_RT64)
    // Backspace and the controller's right shoulder (unbound by default;
    // CONKER_NO_CONTROLLER leaves the keyboard alone).
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const bool backspace = keys != nullptr && keys[SDL_SCANCODE_BACKSPACE] != 0;
    const bool shoulder = recompinput::get_input_digital(0, recompinput::InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    const bool down = backspace || shoulder;

    static bool pressed = false;
    static int frames_left = 0;
    if (down && !pressed && !recompinput::game_input_disabled()) {
        frames_left = press_frames;
    }
    pressed = down;

    const uint32_t game_mode = (uint32_t)MEM_W(0, (gpr)(int32_t)game_mode_pointer);
    const bool intro_movie = game_mode == game_mode_intro_movie;
    conker_skip_pad_buttons = frames_left > 0 ? (intro_movie ? n64_start : n64_l) : 0;

    // TEMP-DEBUG: CONKER_SKIP_LOG prints each press and the section timer once a second.
    static const bool log = SDL_getenv("CONKER_SKIP_LOG") != nullptr;
    if (log) {
        if (conker_skip_pad_buttons != 0 && frames_left == press_frames) {
            std::fprintf(stderr, "[host] skip press: mode %X -> %04X\n", (unsigned)game_mode, (unsigned)conker_skip_pad_buttons);
        }
        static int log_frames = 0;
        if ((log_frames++ % 120) == 0) {
            std::fprintf(stderr, "[host] mode %X play %d el %d/%d\n", (unsigned)game_mode,
                (int)MEM_BU(0, (gpr)(int32_t)0x800C35EA), MEM_W(0, (gpr)(int32_t)0x800C35B0), MEM_W(0, (gpr)(int32_t)0x800C3640));
        }
    }

    if (frames_left > 0) {
        frames_left--;
    }
#endif
}
