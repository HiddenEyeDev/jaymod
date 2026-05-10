// Jaymod-AC: rotating daily challenges.  See g_dailychallenges.h.

#include <bgame/impl.h>
#include "g_dailychallenges.h"

namespace Daily {

///////////////////////////////////////////////////////////////////////////////
// Definition table.  Order MUST match enum Id; persistence keys are stable.
// `metric` is part of the def (not in the public header) — kept private so
// other modules don't have to know about it.

namespace {

struct DefInternal {
    const char* key;
    const char* title;
    const char* descr;
    int         threshold;
    int         xpReward;
    Metric      metric;
    bool        excludeFromPicks;  // reserved entries (e.g. NIGHT_SHIFT)
};

const DefInternal kDefsInternal[NUM] = {
    // key            title            description                            thr   xp    metric         skip
    { "bloodletter",  "Bloodletter",   "Get 100 kills today.",                100,  150, M_KILL,        false },
    { "headhunter",   "Headhunter",    "Get 100 headshots today.",            100,  150, M_HEADSHOT,    false },
    { "fieldmedic",   "Field Medic",   "Revive 40 teammates today.",           40,  200, M_REVIVE,      false },
    { "demolitionist","Demolitionist", "Deal 10000 damage today.",          10000,  150, M_DAMAGE,      false },
    { "persistent",   "Persistent",    "Play for 60 minutes today.",         3600,  100, M_PLAYTIME,    false },
    { "onaroll",      "On a Roll",     "Hit a 5-kill streak today.",            1,  200, M_STREAK_HIT,  false },
    { "slaughterhouse","Slaughterhouse","Land a multi-kill (3+) today.",        1,  250, M_MULTIKILL,   false },
    { "nightshift",   "Night Shift",   "Reserved.",                             1,    0, M_PLAYTIME,    true  },
};

// Public-facing view of the def.  Build a parallel public array on first
// access so callers can iterate kDefs[i] like any other table.
} // namespace

const Def kDefs[NUM] = {
    { kDefsInternal[0].key, kDefsInternal[0].title, kDefsInternal[0].descr, kDefsInternal[0].threshold, kDefsInternal[0].xpReward },
    { kDefsInternal[1].key, kDefsInternal[1].title, kDefsInternal[1].descr, kDefsInternal[1].threshold, kDefsInternal[1].xpReward },
    { kDefsInternal[2].key, kDefsInternal[2].title, kDefsInternal[2].descr, kDefsInternal[2].threshold, kDefsInternal[2].xpReward },
    { kDefsInternal[3].key, kDefsInternal[3].title, kDefsInternal[3].descr, kDefsInternal[3].threshold, kDefsInternal[3].xpReward },
    { kDefsInternal[4].key, kDefsInternal[4].title, kDefsInternal[4].descr, kDefsInternal[4].threshold, kDefsInternal[4].xpReward },
    { kDefsInternal[5].key, kDefsInternal[5].title, kDefsInternal[5].descr, kDefsInternal[5].threshold, kDefsInternal[5].xpReward },
    { kDefsInternal[6].key, kDefsInternal[6].title, kDefsInternal[6].descr, kDefsInternal[6].threshold, kDefsInternal[6].xpReward },
    { kDefsInternal[7].key, kDefsInternal[7].title, kDefsInternal[7].descr, kDefsInternal[7].threshold, kDefsInternal[7].xpReward },
};

// Build-time guards.
//   - bitmask packing: 32 bits in `dailyCompleted`
//   - storage: User::dailyProgress[8] in User.h must have room
typedef int compile_check_NUM_fits_bitmask[ (NUM <= 32) ? 1 : -1 ];
typedef int compile_check_NUM_fits_user_array[ (NUM <= 8) ? 1 : -1 ];

///////////////////////////////////////////////////////////////////////////////

namespace {

// Server-static: tracks the day on which we last broadcast the
// "today's challenges" message, so we only fire it once per UTC day even
// across map rotations.
int s_lastBroadcastDay = -1;

// Cheap LCG seeded by day index so all servers and all players see the
// same daily picks.  Avoids touching global rand() state.
unsigned int dayRand( unsigned int& state, unsigned int range ) {
    state = state * 1103515245u + 12345u;
    return (state >> 16) % range;
}

User* resolveUser( int clientNum ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return NULL;
    User* user = connectedUsers[clientNum];
    if (!user || user->isNull() || user->fakeguid) return NULL;
    const gentity_t* ent = &g_entities[clientNum];
    if (ent->r.svFlags & SVF_BOT) return NULL;
    return user;
}

const char* netname( int clientNum ) {
    const gentity_t* ent = &g_entities[clientNum];
    return (ent->client) ? ent->client->pers.netname : "?";
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

int dayIndex() {
    return (int)(time( NULL ) / 86400);
}

// Deterministic Fisher-Yates pick of 3 from the eligible (non-skip) subset.
// Output indices are values from enum Id.
void getActive( int outIds[3] ) {
    // Build pool of eligible IDs.
    int pool[NUM];
    int n = 0;
    for (int i = 0; i < NUM; ++i) {
        if (!kDefsInternal[i].excludeFromPicks)
            pool[n++] = i;
    }

    // Seed from day index with a multiplicative scramble for better
    // small-input distribution.
    unsigned int state = (unsigned int)dayIndex() * 2654435761u + 1u;

    // Pick min(3, n).
    const int want = (n < 3) ? n : 3;
    for (int k = 0; k < want; ++k) {
        const int idx = (int)dayRand( state, (unsigned int)n );
        outIds[k] = pool[idx];
        pool[idx] = pool[--n];
    }
    // Pad with -1 if pool ran out (shouldn't happen with NUM >= 3 eligible).
    for (int k = want; k < 3; ++k) outIds[k] = -1;
}

///////////////////////////////////////////////////////////////////////////////
// Hooks

void onLevelInit() {
    const int today = dayIndex();
    if (s_lastBroadcastDay == today) return;
    s_lastBroadcastDay = today;

    int active[3];
    getActive( active );

    AP( "chat \"^3*** ^5DAILY CHALLENGES ^3***\"" );
    for (int i = 0; i < 3; ++i) {
        if (active[i] < 0) continue;
        const DefInternal& d = kDefsInternal[active[i]];
        AP( va( "chat \"^5%s^7: %s ^8(+%d XP)\"",
            d.title, d.descr, d.xpReward ) );
    }
}

void rotateIfNeeded( int clientNum ) {
    User* user = resolveUser( clientNum );
    if (!user) return;

    const int today = dayIndex();
    if (user->dailyDayIndex == today) return;

    // Day rolled over (or first time) — wipe progress.
    user->dailyDayIndex   = today;
    user->dailyCompleted  = 0;
    memset( user->dailyProgress, 0, sizeof(user->dailyProgress) );
}

void onProgress( int clientNum, Metric metric, int delta ) {
    if (delta <= 0) return;

    User* user = resolveUser( clientNum );
    if (!user) return;

    rotateIfNeeded( clientNum );

    int active[3];
    getActive( active );

    for (int i = 0; i < 3; ++i) {
        const int id = active[i];
        if (id < 0) continue;

        const DefInternal& d = kDefsInternal[id];
        if (d.metric != metric) continue;

        // Skip if already completed today.
        if (user->dailyCompleted & (1 << id)) continue;

        // Bump.  For boolean-event metrics (M_STREAK_HIT, M_MULTIKILL) the
        // caller passes delta=1 and threshold is 1; one event completes.
        user->dailyProgress[id] += delta;

        if (user->dailyProgress[id] >= d.threshold) {
            // Clamp display & mark completed.
            user->dailyProgress[id] = d.threshold;
            user->dailyCompleted   |= (1 << id);

            // XP bonus: bumps statKills/headshots-derived XP via the rep
            // system?  No — daily XP is a flat reward, recorded in a
            // separate persistent counter.  See User::statDailyXp below.
            user->statDailyXp += d.xpReward;

            AP( va( "chat \"^3*** ^7%s ^7completed daily: ^5%s ^8(+%d XP)\"",
                netname(clientNum), d.title, d.xpReward ) );
        }
    }
}

///////////////////////////////////////////////////////////////////////////////

} // namespace Daily
