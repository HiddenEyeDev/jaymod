// Jaymod-AC: rotating daily challenges.
//
// Every UTC day the server selects a deterministic 3-challenge subset from
// `kDailyDefs[]` (seeded by floor(time/86400)) and broadcasts them.  Players
// get progress tracked against those three; on completion they earn bonus
// XP and a public broadcast.
//
// Progress is per-User and resets at UTC midnight (when Daily::dayIndex()
// changes).  Storage is fixed-size by enum so persistence is robust against
// re-ordering/inserts (handled via named keys, see User.cpp).

#ifndef G_DAILYCHALLENGES_H
#define G_DAILYCHALLENGES_H

class User;

namespace Daily {

///////////////////////////////////////////////////////////////////////////////
// Challenge IDs.  Append at the bottom; never reorder or delete (storage
// is keyed off these enums).

enum Id {
    BLOODLETTER = 0,    // 25 kills today
    HEADHUNTER,         // 8 headshot HITS today
    FIELD_MEDIC,        // 15 revives today
    DEMOLITIONIST,      // 5000 damage today
    PERSISTENT,         // 3600 seconds (1h) playtime today
    ON_A_ROLL,          // hit a 5-kill streak today
    SLAUGHTERHOUSE,     // 1 multi-kill (3+ in 4s) today
    NIGHT_SHIFT,        // 60 minutes spec/playing during off-hours [skipped, reserved]

    NUM
};

///////////////////////////////////////////////////////////////////////////////
// Static definition.

struct Def {
    const char* key;        // persistence id (e.g. "bloodletter")
    const char* title;      // display title
    const char* descr;      // shown in `!daily`
    int         threshold;  // counter target
    int         xpReward;   // XP awarded on completion
};

extern const Def kDefs[NUM];

///////////////////////////////////////////////////////////////////////////////
// Metric kinds. Used by hooks to bump only the matching active challenges.

enum Metric {
    M_KILL,         // every kill (int delta)
    M_HEADSHOT,     // every headshot HIT (int delta)
    M_REVIVE,       // every revive (int delta)
    M_DAMAGE,       // damage dealt (int delta)
    M_PLAYTIME,     // seconds played (int delta)
    M_STREAK_HIT,   // streak counter has reached >= 5 (boolean event)
    M_MULTIKILL,    // multi-kill window count >= 3 (boolean event)
};

///////////////////////////////////////////////////////////////////////////////
// Day index helpers.

int  dayIndex();                          // floor(time(NULL) / 86400)
void getActive ( int outIds[3] );         // today's three picks (deterministic)

///////////////////////////////////////////////////////////////////////////////
// Hooks

// Called from G_InitGame; may broadcast a "Today's daily challenges" line.
void onLevelInit();

// Called from various stat hooks. Bumps progress on any active challenge
// whose metric matches; on completion, broadcasts and awards XP.
void onProgress( int clientNum, Metric metric, int delta );

// Wipes per-user progress if their stored dayIndex is stale. Idempotent.
void rotateIfNeeded( int clientNum );

///////////////////////////////////////////////////////////////////////////////

} // namespace Daily

#endif // G_DAILYCHALLENGES_H
