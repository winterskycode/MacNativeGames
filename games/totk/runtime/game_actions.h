/* The game's own controls (1.4.3, found 2026-10-27): what each Switch button does in the field, so an action can be put on
   another button without moving the menus' confirm / cancel with it.
   The player's input routine (main+0xf23d70) packs the controller's pressed, held and released buttons (sead::Controller
   +0x8, +0x114, +0xc) and turns each button into a fixed set of action bits, OR'd into three 64-bit masks the player code
   reads ([x19+0x28] pressed, [x19+0x38] held, [x19+0x48] released). Each button adds the same bits whatever else is
   down, so a table per button reproduces it exactly (checked against the routine for every button, both settings of the
   game's "Swap Jump Controls", IsChangeJumpButton). That option only moves the jump bits (pressed 0x401, held 0x1) and the
   sprint bits (0x22) between X and B; B keeps 0x2000000 and X 0x4080000 (pressed only). Menus read the buttons
   themselves, not these masks: confirm stays A and cancel B whatever the actions are.
   game_patches_v143.c (patch_actions) replaces the bits with tables: GAME_ACTION_TABLE words per set, the pressed table
   by packed button (14), the held one (12, no stick clicks) and the released one by sead button bit (32).
   Pure C, shared by the Mac and Android hosts. */
#ifndef TOTK_GAME_ACTIONS_H
#define TOTK_GAME_ACTIONS_H
#include <stdint.h>
#include <string.h>
#include <strings.h>

enum { GAME_ACTION_PRESSED = 14, GAME_ACTION_HELD = 12, GAME_ACTION_RELEASED = 32,
       GAME_ACTION_TABLE = GAME_ACTION_PRESSED + GAME_ACTION_HELD + GAME_ACTION_RELEASED };

/* Switch buttons by controller_switch_names index (a b x y lstick rstick l r zl zr plus minus left up right down): the
   packed index in the pressed / held tables (-1: not in it) and the sead button bit of the released one. Plus and minus
   open menus and carry no actions. */
static const int8_t game_action_pressed_index[16] = {0, 1, 2, 3, 12, 13, 4, 5, 6, 7, -1, -1, 10, 8, 11, 9};
static const int8_t game_action_held_index[16]    = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, -1, -1, 10, 8, 11, 9};
static const int8_t game_action_sead_bit[16]      = {0, 1, 3, 4, 7, 6, 13, 14, 2, 5, 10, 9, 18, 16, 19, 17};

/* The actions, with their bits and the Switch button the game gives them (controller_switch_names index). */
typedef struct { const char *name, *label; uint64_t pressed, held, released; int8_t button; } game_action;
enum { GAME_ACTIONS = 17 };
/* indices into game_actions[] (its order) */
enum { GA_JUMP, GA_SPRINT, GA_ATTACK, GA_INTERACT, GA_CANCEL, GA_XACTION, GA_ABILITY, GA_THROW, GA_SHIELD, GA_BOW,
       GA_UP, GA_DOWN, GA_LEFT, GA_RIGHT, GA_CROUCH, GA_SCOPE, GA_NONE };
static const game_action game_actions[GAME_ACTIONS] = {
    {"jump",     "Jump",                      0x401,      0x1,     0,     2},    /* X */
    {"sprint",   "Sprint",                    0x22,       0x22,    0,     1},    /* B */
    {"attack",   "Attack",                    0x200,      0x200,   0x200, 3},    /* Y: charged attacks on release */
    {"interact", "Talk, pick up, open",       0x10090,    0x10010, 0,     0},    /* A */
    {"cancel",   "Put away, cancel",          0x2000000,  0,       0,     1},    /* B's own bit */
    {"xaction",  "Second X action",           0x4080000,  0,       0,     2},    /* X's own bits */
    {"ability",  "Abilities",                 0x1000040,  0x40,    0x40,  6},    /* L */
    {"throw",    "Throw",                     0x5800,     0x5100,  0,     7},    /* R */
    {"shield",   "Shield, lock on",           0x4,        0x4,     0,     8},    /* ZL */
    {"bow",      "Bow",                       0x2000,     0x2000,  0,     9},    /* ZR */
    {"up",       "D-pad up action",           0x100000,   0x100000, 0,    13},
    {"down",     "D-pad down action",         0x240000,   0x240000, 0,    15},
    {"left",     "D-pad left action",         0x400000,   0x400000, 0,    12},
    {"right",    "D-pad right action",        0x800000,   0x800000, 0,    14},
    {"crouch",   "Crouch",                    0x8,        0,       0,     4},    /* left stick click */
    {"scope",    "Scope",                     0x20000,    0,       0,     5},    /* right stick click */
    {"none",     NULL,                        0,          0,       0,     -1},
};

/* The tables for buttons[i] = the Switch button (index) of game_actions[i], -1 for none. Returns 0, or 1 + the index of
   an action whose button cannot carry it (its held bits on a stick click, or plus / minus). */
static inline int game_actions_table(const int8_t buttons[GAME_ACTIONS], uint64_t table[GAME_ACTION_TABLE]) {
    memset(table, 0, GAME_ACTION_TABLE * sizeof *table);
    for (int i = 0; i < GAME_ACTIONS; i++) {
        int b = buttons[i]; const game_action *a = &game_actions[i];
        if (b < 0 || !(a->pressed | a->held | a->released)) continue;
        if (b >= 16 || game_action_pressed_index[b] < 0 || (a->held && game_action_held_index[b] < 0)) return 1 + i;
        table[game_action_pressed_index[b]] |= a->pressed;
        if (a->held) table[GAME_ACTION_PRESSED + game_action_held_index[b]] |= a->held;
        if (a->released) table[GAME_ACTION_PRESSED + GAME_ACTION_HELD + game_action_sead_bit[b]] |= a->released;
    }
    return 0;
}
/* The field prompts (2026-10-27): the control guide at the bottom of the screen (the CtrlGuideSetTable sets, e.g.
   Ultrahand's "Cancel B_OptionX") asks main+0xba29e0 for a glyph by id (1 A, 2 B, 3 X, 4 Y, 5 L, 6 ZL, 7 R, 8 ZR,
   9 Plus, 10 Minus, 11-14 Up Down Left Right, 25 LS_Push, 33 RS_Push, 35 B_OptionX: sprint, 36 X_OptionB: jump). A
   plain letter there is that button's own action (the game keeps them when its option swaps jump and sprint), so each
   glyph stands for one action: the map gives the glyph of the Switch button the action is on now. */
enum { GAME_GLYPHS = 64 };
static const int8_t game_glyph_action[GAME_GLYPHS] = {   /* glyph id -> game_actions index, -1 none */
    [0] = -1, [1] = 3, [2] = 4, [3] = 5, [4] = 2, [5] = 6, [6] = 8, [7] = 7, [8] = 9, [9] = -1, [10] = -1,
    [11] = 10, [12] = 11, [13] = 12, [14] = 13, [15 ... 24] = -1, [25] = 14, [26 ... 32] = -1, [33] = 15, [34] = -1,
    [35] = 1, [36] = 0, [37 ... 63] = -1};
static const uint8_t game_glyph_of_button[16] = {1, 2, 3, 4, 25, 33, 5, 7, 6, 8, 9, 10, 13, 11, 14, 12};   /* by controller_switch_names */
static inline void game_actions_glyphs(const int8_t buttons[GAME_ACTIONS], uint8_t map[GAME_GLYPHS]) {
    for (int g = 0; g < GAME_GLYPHS; g++) {
        int a = game_glyph_action[g], b = a >= 0 ? buttons[a] : -1;
        map[g] = (uint8_t)(b >= 0 && b < 16 ? game_glyph_of_button[b] : g);
    }
}
/* A glyph's character in the button font (External_00): StaticMsg/GuideKeyIcon.msbt, label = glyph name */
static const uint16_t game_glyph_char[37] = {0, 0xe040, 0xe041, 0xe042, 0xe043, 0xe044, 0xe046, 0xe045, 0xe047, 0xe04b, 0xe04c,
    0xe060, 0xe061, 0xe062, 0xe063, 0xe064, 0xe065, 0xe049, 0xe05e, 0xe052, 0xe050, 0xe05a, 0xe058, 0xe05c, 0xe054, 0xe056,
    0xe05f, 0xe053, 0xe051, 0xe05b, 0xe059, 0xe05d, 0xe055, 0xe057, 0xe068, 0, 0};
/* Button icons in message text ({1.4:<id>cd} tags; main+0x20d7808 turns an id into a font character): each id's glyph
   as the game draws it (35 / 36: sprint / jump, B / X or X / B by the game's own option), read from that function. Ids
   23, 24, 38, 39, 40, 43 are menus' (Plus, Minus, the map screen, the option's own text): only the face letters follow
   the face-button setting there. */
enum { GAME_TEXT_ICONS = 45 };
static const uint8_t game_text_icon_glyph[GAME_TEXT_ICONS] = {19, 20, 21, 22, 31, 32, 11, 12, 13, 14, 1, 1, 36, 4, 6, 6, 35,
    35, 35, 35, 5, 7, 8, 9, 10, 0, 0, 0, 0, 0, 0, 0, 0, 18, 26, 24, 0, 36, 3, 2, 3, 0, 7, 15, 0};
static inline int game_text_icon_menu(int id) { return id == 23 || id == 24 || id == 38 || id == 39 || id == 40 || id == 43; }
/* The face letters as printed on the device: with face buttons by position (the Switch layout's places) an Xbox-layout
   device's printed letters are the other two of each pair, A <-> B and X <-> Y */
static inline uint8_t game_glyph_printed(uint8_t g, int swap_faces) {
    static const uint8_t swap[5] = {0, 2, 1, 4, 3};
    return swap_faces && g >= 1 && g <= 4 ? swap[g] : g;
}
/* The text icons' characters: field ones through the field map (NULL: the game's own controls; the jump and sprint
   icons then keep the game's own choice, 0), menus' as they are; then the printed letters */
static inline void game_text_icon_chars(const uint8_t *field, int swap_faces, uint16_t chars[64]) {
    for (int i = 0; i < 64; i++) {
        if (i >= GAME_TEXT_ICONS || !game_text_icon_glyph[i]) { chars[i] = 0; continue; }
        uint8_t g = game_text_icon_glyph[i];
        if (!game_text_icon_menu(i)) { if (field) g = field[g]; else if (g >= 35) { chars[i] = 0; continue; } }
        g = game_glyph_printed(g, swap_faces);
        chars[i] = g < 37 ? game_glyph_char[g] : 0;
    }
}
_Static_assert(GA_NONE + 1 == GAME_ACTIONS, "the GA_ indices follow game_actions[]");
/* The physical button (controller_map.h: south, east, west, north, then the rest by index) that presses Switch button b
   in the Switch layout (east A, south B, north X, west Y); the face swap is its own inverse. -1 stays -1. */
static inline int game_physical_of_switch(int b) { static const int8_t face[4] = {1, 0, 3, 2}; return b < 0 ? -1 : b < 4 ? face[b] : b; }
static inline void game_actions_default(int8_t buttons[GAME_ACTIONS]) { for (int i = 0; i < GAME_ACTIONS; i++) buttons[i] = game_actions[i].button; }
static inline int game_action_index(const char *name, size_t n) {
    for (int i = 0; i < GAME_ACTIONS; i++) if (strlen(game_actions[i].name) == n && !strncasecmp(game_actions[i].name, name, n)) return i;
    return -1;
}
#endif
