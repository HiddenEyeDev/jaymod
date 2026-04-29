// Jaymod-AC: achievements system.
//
// Public API:
//
//   - Ach::onKill   — call from player_die() with attacker, victim, MOD
//   - Ach::onStat   — call after G_Profile_Accrue with clientNum
//   - Ach::onSpawn  — call when a client respawns; resets multi-kill window
//
// Storage: each User carries an int[NUM] counter array.  Non-repeatable
// achievements clamp at 1; repeatable achievements increment freely.
// Persisted in users.db via named keys (`ach.firstBlood = 3`) so the on-disk
// format is robust against re-ordering or insertion of new achievements.

#ifndef G_ACHIEVEMENTS_H
#define G_ACHIEVEMENTS_H

class User;

namespace Ach {

///////////////////////////////////////////////////////////////////////////////
// Identifiers.  ORDER affects the in-memory array index but NOT persistence
// (which uses string keys).  Append new entries at the bottom; do not delete
// or reorder if you care about historical data.

enum Id {
    FIRST_BLOOD = 0,    // first kill ever
    NOVICE,             // 100 lifetime kills
    VETERAN,            // 1000 lifetime kills
    ELITE,              // 5000 lifetime kills
    LEGEND,             // 10000 lifetime kills

    KILLING_SPREE,      // 5-kill streak (in one life)
    RAMPAGE,            // 10-kill streak
    UNSTOPPABLE,        // 15-kill streak
    GODLIKE,            // 20-kill streak
    WICKED_SICK,        // 30-kill streak

    DOUBLE_KILL,        // 2 kills within 4s    (repeatable)
    TRIPLE_KILL,        // 3 kills within 4s    (repeatable)
    MULTI_SLAYER,       // 4 kills within 4s    (repeatable)
    MEGA_KILL,          // 5+ kills within 4s   (repeatable)

    SHARPSHOOTER,       // 25% acc, 500+ shots
    DEADEYE,            // 35% acc, 1000+ shots
    SNIPER_SUPREME,     // 50% acc, 500+ shots

    HEADHUNTER,         // 100 headshots
    DECAPITATOR,        // 500 headshots
    MIND_READER,        // 1000 headshots

    SURVIVOR,           // K/D 1.5 with 100+ kills
    UNTOUCHABLE,        // K/D 2.5 with 100+ kills
    DOMINATOR,          // K/D 5.0 with 250+ kills

    ROOKIE,             // 1h playtime
    REGULAR,            // 10h playtime
    ENTHUSIAST,         // 50h playtime
    ADDICTED,           // 100h playtime
    NO_LIFE,            // 500h playtime

    REVENGE,            // kill the player who last killed you   (repeatable)
    FIRST_FRAG,         // first kill of a round                  (repeatable)

    NUM
};

///////////////////////////////////////////////////////////////////////////////
// Static definition for one achievement.

struct Def {
    const char* key;        // persistence key (case-insensitive when read)
    const char* title;      // display title shown in broadcast
    const char* descr;      // tooltip / help text
    bool        repeatable; // award broadcasts on every increment when true
};

extern const Def kDefs[NUM];

///////////////////////////////////////////////////////////////////////////////
// Hooks

void onLevelInit();
void onKill     ( gentity_t* attacker, gentity_t* victim, int mod );
void onStat     ( int clientNum );
void onSpawn    ( int clientNum );

///////////////////////////////////////////////////////////////////////////////

} // namespace Ach

#endif // G_ACHIEVEMENTS_H
