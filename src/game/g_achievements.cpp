// Jaymod-AC: achievements system.  See g_achievements.h for the public API.

#include <bgame/impl.h>
#include "g_achievements.h"
#include "g_profile.h"

namespace Ach {

///////////////////////////////////////////////////////////////////////////////
// Definition table.  Order MUST match enum Id; persistence keys are stable.
//
// Adding a new achievement:
//   1. add the enum value in g_achievements.h (append at the bottom)
//   2. add a row here (key must be unique, lowercase preferred)
//   3. add award logic in onKill() or onStat()

const Def kDefs[NUM] = {
    // key                  title                 description                                              repeatable
    { "firstBlood",     "First Blood",        "Get your first kill.",                                  false },
    { "novice",         "Novice",             "Reach 100 lifetime kills.",                             false },
    { "veteran",        "Veteran",            "Reach 1000 lifetime kills.",                            false },
    { "elite",          "Elite",              "Reach 5000 lifetime kills.",                            false },
    { "legend",         "Legend",             "Reach 10000 lifetime kills.",                           false },

    { "killingSpree",   "Killing Spree",      "5 kills without dying.",                                false },
    { "rampage",        "Rampage",            "10 kills without dying.",                               false },
    { "unstoppable",    "Unstoppable",        "15 kills without dying.",                               false },
    { "godlike",        "Godlike",            "20 kills without dying.",                               false },
    { "wickedSick",     "Wicked Sick",        "30 kills without dying.",                               false },

    { "doubleKill",     "Double Kill",        "2 kills within 4 seconds.",                             true  },
    { "tripleKill",     "Triple Kill",        "3 kills within 4 seconds.",                             true  },
    { "multiSlayer",    "MultiSlayer",        "4 kills within 4 seconds.",                             true  },
    { "megaKill",       "Mega Kill",          "5+ kills within 4 seconds.",                            true  },

    { "sharpshooter",   "Sharpshooter",       "25%+ accuracy with 500+ shots fired.",                  false },
    { "deadeye",        "Deadeye",            "35%+ accuracy with 1000+ shots fired.",                 false },
    { "sniperSupreme",  "Sniper Supreme",     "50%+ accuracy with 500+ shots fired.",                  false },

    { "headhunter",     "Headhunter",         "100 lifetime headshots.",                               false },
    { "decapitator",    "Decapitator",        "500 lifetime headshots.",                               false },
    { "mindReader",     "Mind Reader",        "1000 lifetime headshots.",                              false },

    { "survivor",       "Survivor",           "K/D 1.5+ with 100+ lifetime kills.",                    false },
    { "untouchable",    "Untouchable",        "K/D 2.5+ with 100+ lifetime kills.",                    false },
    { "dominator",      "Dominator",          "K/D 5.0+ with 250+ lifetime kills.",                    false },

    { "rookie",         "Rookie",             "1 hour of total playtime.",                             false },
    { "regular",        "Regular",            "10 hours of total playtime.",                           false },
    { "enthusiast",     "Enthusiast",         "50 hours of total playtime.",                           false },
    { "addicted",       "Addicted",           "100 hours of total playtime.",                          false },
    { "noLife",         "No Life",            "500 hours of total playtime.",                          false },

    { "revenge",        "Revenge",            "Kill the player who last killed you.",                  true  },
    { "firstFrag",      "First Frag",         "First kill of the round.",                              true  },
};

// Build-time guard: NUM must fit in User::ACH_MAX storage.
typedef int compile_time_check_NUM_fits_in_ACH_MAX[ (NUM <= 64) ? 1 : -1 ];

///////////////////////////////////////////////////////////////////////////////

namespace {

// Reset on every G_InitGame (per map).  Per-round-restart resets aren't done
// here — close enough that "first frag of the map" matches player intuition.
qboolean s_firstFragSeen = qfalse;

// Resolve to a real, mutable User record we should write achievements to.
User* resolveUser( gentity_t* ent ) {
    if (!ent || !ent->client) return NULL;
    if (ent->r.svFlags & SVF_BOT) return NULL;

    const int slot = ent->s.number;
    if (slot < 0 || slot >= MAX_CLIENTS) return NULL;

    User* user = connectedUsers[slot];
    if (!user || user->isNull() || user->fakeguid) return NULL;
    return user;
}

const char* netname( const gentity_t* ent ) {
    return (ent && ent->client) ? ent->client->pers.netname : "?";
}

// Award one increment of an achievement.  For non-repeatable achievements
// the second+ call is a no-op (no broadcast).  Broadcast is `chat`-style so
// it appears alongside other server announcements.
void award( gentity_t* ent, Id id ) {
    if (id < 0 || id >= NUM) return;

    User* user = resolveUser( ent );
    if (!user) return;

    const Def& def = kDefs[id];
    int& counter = user->achCount[id];

    if (!def.repeatable && counter > 0)
        return;  // already unlocked — silent

    counter++;

    // Broadcast.  Non-repeatable: "unlocked".  Repeatable: "[Nx] earned".
    if (def.repeatable) {
        AP( va( "chat \"^3*** ^7%s ^7earned: ^5[%dx] %s\"",
            netname(ent), counter, def.title ) );
    } else {
        AP( va( "chat \"^3*** ^7%s ^7unlocked: ^5%s\"",
            netname(ent), def.title ) );
    }

    // Achievement bonus XP may have pushed the player into a new rank.
    G_Profile_CheckRankUp( ent->s.number );
}

// Convenience: award only if the current counter is below `floor` (i.e.
// haven't unlocked it yet OR repeatable hasn't reached this level yet).
void awardOnce( gentity_t* ent, Id id ) {
    User* user = resolveUser( ent );
    if (!user) return;
    if (user->achCount[id] > 0) return;
    award( ent, id );
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// Public hooks

void onLevelInit() {
    s_firstFragSeen = qfalse;
}

// ---------------------------------------------------------------------------
// onKill: called from player_die() AFTER G_Profile_OnDeath has updated
// streak counters.  Awards streak/multi-kill/revenge/first-frag categories.
// Lifetime-stat thresholds (kills, headshots, K/D, etc.) are awarded from
// onStat after G_Profile_Accrue folds session deltas into the User record.
// ---------------------------------------------------------------------------
void onKill( gentity_t* attacker, gentity_t* victim, int /*mod*/ ) {
    if (!attacker || !attacker->client) return;
    if (!victim || !victim->client)     return;
    if (attacker == victim)              return;   // self-kill
    if (attacker->r.svFlags & SVF_BOT)   return;

    User* attUser = resolveUser( attacker );
    if (!attUser) return;

    // First frag of the map.
    if (!s_firstFragSeen) {
        s_firstFragSeen = qtrue;
        award( attacker, FIRST_FRAG );
    }

    // Revenge: this victim is the player who last killed the attacker.
    if (attacker->client->lastkilledby_client == victim->s.number) {
        award( attacker, REVENGE );
    }

    // Multi-kill window
    {
        gclient_t* cl = attacker->client;
        if (level.time < cl->pers.multiKillExpireTime) {
            cl->pers.multiKillCount++;
        } else {
            cl->pers.multiKillCount = 1;
        }
        cl->pers.multiKillExpireTime = level.time + 4000;

        switch (cl->pers.multiKillCount) {
            case 2: award( attacker, DOUBLE_KILL );  break;
            case 3: award( attacker, TRIPLE_KILL );  break;
            case 4: award( attacker, MULTI_SLAYER ); break;
            case 5: award( attacker, MEGA_KILL );    break;
            // 6+ in one window: stay at Mega tier; don't re-broadcast.
        }
    }

    // Streak achievements (currentStreak was just incremented in
    // G_Profile_OnDeath which runs immediately before us).
    const int s = attacker->client->pers.currentStreak;
    switch (s) {
        case 5:  awardOnce( attacker, KILLING_SPREE ); break;
        case 10: awardOnce( attacker, RAMPAGE );       break;
        case 15: awardOnce( attacker, UNSTOPPABLE );   break;
        case 20: awardOnce( attacker, GODLIKE );       break;
        case 30: awardOnce( attacker, WICKED_SICK );   break;
        // For values > 30 or non-milestone counts, nothing to do; the
        // !profile longest-streak number speaks for itself.
    }
}

// ---------------------------------------------------------------------------
// onStat: called after G_Profile_Accrue.  Walks every threshold-driven
// achievement and awards anything newly satisfied.  Idempotent — already
// unlocked entries are silently skipped by award().
// ---------------------------------------------------------------------------
void onStat( int clientNum ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return;

    gentity_t* ent = &g_entities[clientNum];
    User*      u   = resolveUser( ent );
    if (!u) return;

    // Lifetime kill milestones
    if (u->statKills >= 1)     awardOnce( ent, FIRST_BLOOD );
    if (u->statKills >= 100)   awardOnce( ent, NOVICE );
    if (u->statKills >= 1000)  awardOnce( ent, VETERAN );
    if (u->statKills >= 5000)  awardOnce( ent, ELITE );
    if (u->statKills >= 10000) awardOnce( ent, LEGEND );

    // Headshots
    if (u->statHeadshots >= 100)  awardOnce( ent, HEADHUNTER );
    if (u->statHeadshots >= 500)  awardOnce( ent, DECAPITATOR );
    if (u->statHeadshots >= 1000) awardOnce( ent, MIND_READER );

    // Accuracy (only meaningful with sample size)
    {
        const float acc = u->computeAccuracy();
        if (u->statShotsFired >= 500  && acc >= 0.25f) awardOnce( ent, SHARPSHOOTER );
        if (u->statShotsFired >= 1000 && acc >= 0.35f) awardOnce( ent, DEADEYE );
        if (u->statShotsFired >= 500  && acc >= 0.50f) awardOnce( ent, SNIPER_SUPREME );
    }

    // K/D ratio
    if (u->statKills >= 100 || u->statKills >= 250) {
        const float kd = (u->statDeaths > 0)
            ? (float)u->statKills / (float)u->statDeaths
            : (float)u->statKills;

        if (u->statKills >= 100 && kd >= 1.5f) awardOnce( ent, SURVIVOR );
        if (u->statKills >= 100 && kd >= 2.5f) awardOnce( ent, UNTOUCHABLE );
        if (u->statKills >= 250 && kd >= 5.0f) awardOnce( ent, DOMINATOR );
    }

    // Playtime (seconds)
    if (u->statPlaytimeSecs >= 3600)    awardOnce( ent, ROOKIE );
    if (u->statPlaytimeSecs >= 36000)   awardOnce( ent, REGULAR );
    if (u->statPlaytimeSecs >= 180000)  awardOnce( ent, ENTHUSIAST );
    if (u->statPlaytimeSecs >= 360000)  awardOnce( ent, ADDICTED );
    if (u->statPlaytimeSecs >= 1800000) awardOnce( ent, NO_LIFE );
}

// ---------------------------------------------------------------------------
// onSpawn: called from ClientSpawn.  Resets multi-kill window so the next
// life starts fresh.  (Streak resetting is handled in G_Profile_OnDeath.)
// ---------------------------------------------------------------------------
void onSpawn( int clientNum ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return;
    gclient_t* cl = &level.clients[clientNum];
    cl->pers.multiKillCount       = 0;
    cl->pers.multiKillExpireTime  = 0;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace Ach
