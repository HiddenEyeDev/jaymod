// Jaymod-AC: per-User lifetime stats hooks.
//
// The User class itself owns the persistent fields (statKills, statDeaths,
// statHeadshots, statLongestStreak, statPlaytimeSecs, firstSeen).  This
// module is the glue that funnels in-game events into those fields:
//
//   - G_Profile_OnConnect:  call once per ClientConnect firstTime
//   - G_Profile_OnDeath:    call from player_die() to update streaks
//   - G_Profile_Accrue:     call from disconnect / session-write to fold
//                           sess.{kills,deaths,headshots} deltas + playtime
//                           into the persistent record
//
// Snapshot fields live on clientPersistant_t (see g_local.h) so we never
// double-count and we never miss seconds of playtime, even across map
// changes or unclean disconnects.

#ifndef G_PROFILE_H
#define G_PROFILE_H

void G_Profile_OnConnect ( int clientNum );
void G_Profile_OnDeath   ( gentity_t* victim, gentity_t* attacker );
void G_Profile_Accrue    ( int clientNum );

// Re-checks the rank a User has just earned and, if it's higher than the
// cached `statRank`, broadcasts a "reached Rank N" line and writes the new
// value back. Idempotent. Safe to call repeatedly.
void G_Profile_CheckRankUp( int clientNum );

// Walks all connected clients at intermission, picks winners in several
// categories, and broadcasts an "ROUND MVP" chat block.  No-op if the
// server has fewer than 2 active players.
void G_Profile_AnnounceMVP();

#endif // G_PROFILE_H
