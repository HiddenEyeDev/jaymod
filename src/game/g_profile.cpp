// Jaymod-AC: per-User lifetime stats hooks.  See g_profile.h for design.

#include <bgame/impl.h>
#include "g_profile.h"
#include "g_achievements.h"

namespace {

// Sum shots fired ("atts") and hits across every weapon slot for a session.
// Splash-damage weapons (panzer/mortar/grenades) are included — Phase 2 keeps
// the math simple; we can filter to "accuracy weapons" in a later refinement
// if pure-rifle players want their numbers untainted.
void sumWeaponStats( const gclient_t* cl, int& atts, int& hits ) {
    atts = 0;
    hits = 0;
    for (int w = 0; w < WS_MAX; w++) {
        atts += (int)cl->sess.aWeaponStats[w].atts;
        hits += (int)cl->sess.aWeaponStats[w].hits;
    }
}

// Resolve to a real, mutable User record we should write stats to.
// Returns NULL for slots that we should skip (no client, bot, fakeguid,
// or the BAD sentinel).
User* resolveUser( int clientNum ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return NULL;

    User* user = connectedUsers[clientNum];
    if (!user || user->isNull() || user->fakeguid) return NULL;

    const gentity_t* ent = &g_entities[clientNum];
    if (ent->r.svFlags & SVF_BOT) return NULL;

    return user;
}

} // namespace

// ---------------------------------------------------------------------------
// On first-time connect: stamp firstSeen if unset, anchor playtime accrual,
// and snapshot session counters so we don't double-count anything that may
// already be on the gclient_t (rare but possible after a map_restart).
// ---------------------------------------------------------------------------
void
G_Profile_OnConnect( int clientNum )
{
    User* user = resolveUser( clientNum );
    if (!user) return;

    const time_t now = time( NULL );

    if (user->firstSeen == 0)
        user->firstSeen = now;

    gclient_t* cl = &level.clients[clientNum];
    cl->pers.statsAccrueAnchor  = now;
    cl->pers.statKillsSnap      = cl->sess.kills;
    cl->pers.statDeathsSnap     = cl->sess.deaths;
    cl->pers.statHeadshotsSnap  = cl->sess.headshots;
    cl->pers.currentStreak      = 0;

    int atts, hits;
    sumWeaponStats( cl, atts, hits );
    cl->pers.statShotsFiredSnap = atts;
    cl->pers.statShotsHitSnap   = hits;
}

// ---------------------------------------------------------------------------
// On every player_die: update streak state.  Lifetime kill/death/headshot
// totals are NOT updated here — they're rolled in via G_Profile_Accrue from
// the session counters, which are already maintained by existing code.
// ---------------------------------------------------------------------------
void
G_Profile_OnDeath( gentity_t* victim, gentity_t* attacker )
{
    if (!victim || !victim->client) return;

    // Victim: streak resets.
    victim->client->pers.currentStreak = 0;

    // Attacker: only counts as a streak event if it's a real player kill,
    // not a self-kill / world / environmental death.
    if (!attacker || !attacker->client || attacker == victim) return;
    if (attacker->r.svFlags & SVF_BOT) return;

    attacker->client->pers.currentStreak++;

    User* attUser = resolveUser( attacker->s.number );
    if (!attUser) return;

    if (attacker->client->pers.currentStreak > attUser->statLongestStreak)
        attUser->statLongestStreak = attacker->client->pers.currentStreak;
}

// ---------------------------------------------------------------------------
// Fold sess.{kills,deaths,headshots} deltas + elapsed wall time into the
// persistent User record.  Re-anchors so the next call only accrues new
// activity.  Safe to call repeatedly.
// ---------------------------------------------------------------------------
void
G_Profile_Accrue( int clientNum )
{
    User* user = resolveUser( clientNum );
    if (!user) return;

    gclient_t* cl = &level.clients[clientNum];

    // Playtime
    const time_t now    = time( NULL );
    const time_t anchor = cl->pers.statsAccrueAnchor;
    if (anchor > 0 && now > anchor) {
        // Cap absurd deltas (e.g. wall-clock jump) at 24h to avoid corrupt
        // playtime numbers from system time changes.
        time_t delta = now - anchor;
        if (delta > 86400) delta = 86400;
        user->statPlaytimeSecs += (int)delta;
    }
    cl->pers.statsAccrueAnchor = now;

    // Stat deltas
    const int dKills     = cl->sess.kills      - cl->pers.statKillsSnap;
    const int dDeaths    = cl->sess.deaths     - cl->pers.statDeathsSnap;
    const int dHeadshots = (int)cl->sess.headshots - cl->pers.statHeadshotsSnap;

    if (dKills > 0)     user->statKills     += dKills;
    if (dDeaths > 0)    user->statDeaths    += dDeaths;
    if (dHeadshots > 0) user->statHeadshots += dHeadshots;

    cl->pers.statKillsSnap     = cl->sess.kills;
    cl->pers.statDeathsSnap    = cl->sess.deaths;
    cl->pers.statHeadshotsSnap = cl->sess.headshots;

    // Accuracy: sum across every weapon slot, accrue against the last snapshot.
    int atts, hits;
    sumWeaponStats( cl, atts, hits );

    const int dAtts = atts - cl->pers.statShotsFiredSnap;
    const int dHits = hits - cl->pers.statShotsHitSnap;

    if (dAtts > 0) user->statShotsFired += dAtts;
    if (dHits > 0) user->statShotsHit   += dHits;

    cl->pers.statShotsFiredSnap = atts;
    cl->pers.statShotsHitSnap   = hits;

    // Re-evaluate stat-threshold achievements now that lifetime totals
    // have been bumped.  Idempotent — already-unlocked entries are skipped.
    Ach::onStat( clientNum );

    // After both stats AND achievements may have moved, see if the player
    // has crossed a rank threshold.  Achievements feed into XP, so this
    // must run after Ach::onStat.
    G_Profile_CheckRankUp( clientNum );
}

// ---------------------------------------------------------------------------
// Rank-up detection.  Compares the freshly-computed rank against the cached
// statRank; on increase, broadcasts a chat line and updates the cache so we
// don't re-broadcast next time.  Multi-step jumps (rare — only happen on
// large achievement unlocks or stat backfills) broadcast just the final
// destination rank, not every intermediate.
// ---------------------------------------------------------------------------
void
G_Profile_CheckRankUp( int clientNum )
{
    User* user = resolveUser( clientNum );
    if (!user) return;

    const int newRank = user->computeRank();
    if (newRank <= user->statRank) return;

    user->statRank = newRank;

    const gentity_t* ent = &g_entities[clientNum];
    const char* name = (ent->client) ? ent->client->pers.netname : "?";

    AP( va( "chat \"^3*** ^7%s ^7reached ^5Rank %d^7!\"", name, newRank ) );
}

// ---------------------------------------------------------------------------
// MVP broadcast (Phase 5).
//
// Categories:
//   - Top fragger      : highest sess.kills
//   - Best damage      : highest sess.damage_given
//   - Best medic       : highest sess.revives
//   - Best engineer    : highest sess.skillpoints[SK_EXPLOSIVES_AND_CONSTRUCTION]
//   - Best covertops   : highest sess.skillpoints[SK_MILITARY_INTELLIGENCE_AND_SCOPED_WEAPONS]
//
// Skips entries with zero contribution so we don't print "best medic: 0"
// rounds where nobody played medic.
// ---------------------------------------------------------------------------
namespace {

struct MvpRow {
    int  best;          // highest value seen
    int  bestClient;    // sortedClients index, -1 = none
};

void mvpInit( MvpRow& r ) { r.best = 0; r.bestClient = -1; }

void mvpConsider( MvpRow& r, int client, int value ) {
    if (value > r.best) { r.best = value; r.bestClient = client; }
}

void mvpEmit( const char* label, const MvpRow& r ) {
    if (r.bestClient < 0 || r.best <= 0) return;
    const gentity_t* ent = &g_entities[r.bestClient];
    if (!ent->client) return;
    AP( va( "chat \"^3*** ^5%s^7: %s ^7(%d)\"",
        label, ent->client->pers.netname, r.best ) );
}

} // namespace

void
G_Profile_AnnounceMVP()
{
    if (level.numConnectedClients < 2) return;

    MvpRow fragger, damage, medic, engineer, covertops;
    mvpInit( fragger );
    mvpInit( damage );
    mvpInit( medic );
    mvpInit( engineer );
    mvpInit( covertops );

    for (int i = 0; i < level.numConnectedClients; ++i) {
        const int slot = level.sortedClients[i];
        const gentity_t* ent = &g_entities[slot];
        if (!ent->client) continue;

        // Skip pure spectators — their session counters are unhelpful here.
        const team_t team = ent->client->sess.sessionTeam;
        if (team != TEAM_AXIS && team != TEAM_ALLIES) continue;

        const clientSession_t& s = ent->client->sess;
        mvpConsider( fragger,   slot, (int)s.kills );
        mvpConsider( damage,    slot, (int)s.damage_given );
        mvpConsider( medic,     slot, (int)s.revives );
        mvpConsider( engineer,  slot, (int)s.skillpoints[SK_EXPLOSIVES_AND_CONSTRUCTION] );
        mvpConsider( covertops, slot, (int)s.skillpoints[SK_MILITARY_INTELLIGENCE_AND_SCOPED_WEAPONS] );
    }

    AP( "chat \"^3*** ^5ROUND MVP ^3***\"" );
    mvpEmit( "Top Fragger",  fragger );
    mvpEmit( "Best Damage",  damage );
    mvpEmit( "Best Medic",   medic );
    mvpEmit( "Best Engineer",engineer );
    mvpEmit( "Best CovertOps",covertops );
}
