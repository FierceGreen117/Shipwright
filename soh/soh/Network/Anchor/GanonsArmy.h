#pragma once

/**
 * Ganon's Army
 *
 * An Anchor game mode where one player plays as Link and everyone else joins as Ganon's Army.
 * Army players take control of enemies (Stalfos and Iron Knuckles for now) and use them to stop Link.
 *
 * How it works:
 * - Link's game is in charge of every enemy. The real enemies, hits and damage all happen on Link's game.
 * - Link's game sends each enemy's position and pose to the army players (GA_ENEMY_STATE), whose games
 *   show mirrored copies of those enemies instead of running their own enemy AI.
 * - An army player claims a free enemy (GA_CLAIM) and then streams their controller input to Link's game
 *   (GA_INPUT). Link's game feeds that input to the enemy in place of its AI.
 *
 * This header is usable from C so the enemy actors can ask for puppet input.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <libultraship/libultra.h>

typedef enum {
    GANONS_ARMY_ROLE_OFF = 0,
    GANONS_ARMY_ROLE_LINK = 1,
    GANONS_ARMY_ROLE_ARMY = 2,
} GanonsArmyRole;

typedef struct {
    s16 yaw;     // World direction the army player is pushing the stick towards
    f32 mag;     // Stick strength, 0.0 to 1.0
    u16 held;    // Buttons currently held (BTN_* bits)
    u16 pressed; // Buttons pressed since last frame (BTN_* bits)
} GanonsArmyPuppetInput;

struct Actor;

// Called by enemy actors on Link's game. Returns true (and fills `out`) when an army player is controlling `actor`.
// Safe to call several times per frame; button presses are only reported once per frame.
s32 GanonsArmy_GetPuppetInput(struct Actor* actor, GanonsArmyPuppetInput* out);

#ifdef __cplusplus
}
#endif
