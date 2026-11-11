/* 1.4.3 main: who owns which bytes of the padding after the code (main+0x2bb9c00 to the read-only data at main+0x2bba000,
   1024 bytes). Every veneer the 1.4.3 patches write there takes its offset from this table, and the checks below stop the
   build when two of them overlap or one runs past the padding. Before 2026-11-10 each patch kept its own number in a
   comment; the button prompt cache (PR #1940) took +896, inside the Vert+ stubs (+320 to +928), so a VR run, the only
   one that writes the Vert+ stubs, stopped at start ("aspect patch: text padding for Vert+ in use"). A new veneer gets a
   row here. Free: +304 to +320 and +1008 to +1024. */
#pragma once

enum {
    PAD_V143_FRAME_STEP = 0,     PAD_V143_FRAME_STEP_LEN = 16,     /* real-time frame step (patch_realtime) */
    PAD_V143_OUTPUT = 16,        PAD_V143_OUTPUT_LEN = 28,         /* output resolution (patch_output_res) */
    PAD_V143_ASPECT = 48,        PAD_V143_ASPECT_LEN = 4 * 32,     /* ultrawide: 4 sites x ASPECT_STUB */
    PAD_V143_CLOTH = 176,        PAD_V143_CLOTH_LEN = 19 * 4,      /* cloth / hair clock (patch_secondary_animation) */
    PAD_V143_BOUNCE = 256,       PAD_V143_BOUNCE_LEN = 20,         /* trampoline bounce (patch_slowmo_bounce) */
    PAD_V143_PROMPT = 288,       PAD_V143_PROMPT_LEN = 16,         /* button prompt cache (prompt_cache_install) */
    PAD_V143_VERT = 320,         PAD_V143_VERT_LEN = 2 * 304,      /* Vert+: 2 cameras x ASPECT_VSTUB (VR head look) */
    PAD_V143_FADE = 928,         PAD_V143_FADE_LEN = 80,           /* LOD fade increment (patch_fade_increment) */
    PAD_V143_SIZE = 1024,                                          /* main+0x2bb9c00 to main+0x2bba000 */
};

_Static_assert(PAD_V143_FRAME_STEP + PAD_V143_FRAME_STEP_LEN <= PAD_V143_OUTPUT, "1.4.3 padding: frame step overlaps");
_Static_assert(PAD_V143_OUTPUT + PAD_V143_OUTPUT_LEN <= PAD_V143_ASPECT, "1.4.3 padding: output overlaps");
_Static_assert(PAD_V143_ASPECT + PAD_V143_ASPECT_LEN <= PAD_V143_CLOTH, "1.4.3 padding: aspect overlaps");
_Static_assert(PAD_V143_CLOTH + PAD_V143_CLOTH_LEN <= PAD_V143_BOUNCE, "1.4.3 padding: cloth overlaps");
_Static_assert(PAD_V143_BOUNCE + PAD_V143_BOUNCE_LEN <= PAD_V143_PROMPT, "1.4.3 padding: bounce overlaps");
_Static_assert(PAD_V143_PROMPT + PAD_V143_PROMPT_LEN <= PAD_V143_VERT, "1.4.3 padding: prompt cache overlaps");
_Static_assert(PAD_V143_VERT + PAD_V143_VERT_LEN <= PAD_V143_FADE, "1.4.3 padding: Vert+ overlaps");
_Static_assert(PAD_V143_FADE + PAD_V143_FADE_LEN <= PAD_V143_SIZE, "1.4.3 padding: fade runs past the padding");
_Static_assert(PAD_V143_PROMPT % 8 == 0, "1.4.3 padding: the prompt veneer's address literal must be 8-byte aligned");

/* The field prompt veneers (button prompts and controls by action) share the space of the button prompt cache and the
   Vert+ stubs: wherever they are written, the prompt cache is left out (Android) and Vert+ is never written (no VR). */
enum {
    PAD_V143_ACTIONS = 288,      PAD_V143_ACTIONS_LEN = 41 * 4,    /* field controls by action (patch_actions) */
    PAD_V143_GLYPH_TRACE = 480,  PAD_V143_GLYPH_TRACE_LEN = 11 * 4, /* glyph name trace (patch_glyph_trace) */
    PAD_V143_TEXT_ICONS = 576,   PAD_V143_TEXT_ICONS_LEN = 18 * 4, /* text icons (patch_text_icons) */
    PAD_V143_GLYPHS = 656,       PAD_V143_GLYPHS_LEN = 27 * 4,     /* field prompt glyphs (patch_glyphs) */
};
_Static_assert(PAD_V143_BOUNCE + PAD_V143_BOUNCE_LEN <= PAD_V143_ACTIONS, "1.4.3 padding: bounce overlaps the field controls");
_Static_assert(PAD_V143_ACTIONS + PAD_V143_ACTIONS_LEN <= PAD_V143_GLYPH_TRACE, "1.4.3 padding: field controls overlap");
_Static_assert(PAD_V143_GLYPH_TRACE + PAD_V143_GLYPH_TRACE_LEN <= PAD_V143_TEXT_ICONS, "1.4.3 padding: glyph trace overlaps");
_Static_assert(PAD_V143_TEXT_ICONS + PAD_V143_TEXT_ICONS_LEN <= PAD_V143_GLYPHS, "1.4.3 padding: text icons overlap");
_Static_assert(PAD_V143_GLYPHS + PAD_V143_GLYPHS_LEN <= PAD_V143_FADE, "1.4.3 padding: field prompt glyphs overlap the fade");
