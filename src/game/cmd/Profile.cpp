#include <bgame/impl.h>

namespace cmd {

///////////////////////////////////////////////////////////////////////////////

Profile::Profile()
    : AbstractBuiltin( "profile", true /* grantAlways: anyone can view profiles */ )
{
    __usage << xvalue( "!" + _name ) << ' ' << _ovalue( "PLAYER" );
    __descr << "Show lifetime stats and skill rating for a player (or yourself).";
}

///////////////////////////////////////////////////////////////////////////////

Profile::~Profile()
{
}

///////////////////////////////////////////////////////////////////////////////

AbstractCommand::PostAction
Profile::doExecute( Context& txt )
{
    if (txt._args.size() > 2)
        return PA_USAGE;

    // Resolve target user.  Order of precedence:
    //   1. No arg                  -> self (caller's connected user)
    //   2. Arg matches online name -> that connected user (live, current)
    //   3. Arg matches DB name     -> persistent user record (offline)
    const User* user = NULL;
    string      displayName;

    if (txt._args.size() == 1) {
        if (!txt._client) {
            txt._ebuf << "Console must specify a " << xvalue( "PLAYER" ) << " name.";
            return PA_ERROR;
        }
        user = connectedUsers[txt._client->slot];
        displayName = user ? user->namex : txt._client->gclient.pers.netname;
    }
    else {
        // Try online lookup first.  If it fails, suppress its error and try
        // the persistent User DB by name so offline profiles still work.
        Client* target = NULL;
        if (!lookupPLAYER( txt._args[1], txt, target )) {
            user = connectedUsers[target->slot];
            displayName = user ? user->namex : target->gclient.pers.netname;
        }
        else {
            // lookupPLAYER may have written to _ebuf — clear it before trying DB.
            txt._ebuf.reset();

            const string name = SanitizeString( txt._args[1], false );
            if (name.empty()) {
                txt._ebuf << xvalue( "PLAYER" ) << " is empty.";
                return PA_ERROR;
            }

            string err;
            list<User*> users;
            if (userDB.fetchByName( name, users, err ) || users.empty()) {
                txt._ebuf << "No player or profile found matching " << xvalue( name ) << ".";
                return PA_ERROR;
            }
            // Take the first (most recent) match — same convention as !seen.
            user = *users.begin();
            displayName = user ? user->namex : name;
        }
    }

    if (!user || user->isNull()) {
        txt._ebuf << "No profile data available.";
        return PA_ERROR;
    }

    // Derived numbers
    const int kills     = user->statKills;
    const int deaths    = user->statDeaths;
    const int hsHits    = user->statHeadshots;
    const int streak    = user->statLongestStreak;
    const int playSecs  = user->statPlaytimeSecs;
    const int rating    = user->computeSkillRating();

    const float kd = (deaths > 0)
        ? (float)kills / (float)deaths
        : (float)kills;

    // Headshot % is hits-vs-deaths-of-others (a rough proxy until accuracy
    // tracking lands in Phase 2).  Cap denominator to avoid /0.
    const int   hsDenom = kills > 0 ? kills : 1;
    const float hsPct   = 100.0f * (float)hsHits / (float)hsDenom;

    char kdBuf[16];
    Com_sprintf( kdBuf, sizeof(kdBuf), "%.2f", kd );

    char hsPctBuf[16];
    Com_sprintf( hsPctBuf, sizeof(hsPctBuf), "%.1f", hsPct );

    const float acc      = user->computeAccuracy();
    const int   shotsF   = user->statShotsFired;
    const int   shotsH   = user->statShotsHit;
    char accPctBuf[16];
    Com_sprintf( accPctBuf, sizeof(accPctBuf), "%.1f", acc * 100.f );

    // Format playtime as "Xd Yh Zm" or "Yh Zm" or "Zm".
    char playtimeBuf[64];
    {
        int s = playSecs;
        const int d = s / 86400; s %= 86400;
        const int h = s / 3600;  s %= 3600;
        const int m = s / 60;
        if (d > 0)      Com_sprintf( playtimeBuf, sizeof(playtimeBuf), "%dd %dh %dm", d, h, m );
        else if (h > 0) Com_sprintf( playtimeBuf, sizeof(playtimeBuf), "%dh %dm", h, m );
        else            Com_sprintf( playtimeBuf, sizeof(playtimeBuf), "%dm", m );
    }

    char firstSeenBuf[32];
    if (user->firstSeen) {
        time_t t = user->firstSeen;
        strftime( firstSeenBuf, sizeof(firstSeenBuf), "%Y-%m-%d", localtime( &t ));
    } else {
        Q_strncpyz( firstSeenBuf, "unknown", sizeof(firstSeenBuf) );
    }

    // Layout
    InlineText colA;
    InlineText colB = xvalue;

    colA.flags  |= ios::left;
    colA.width   = 16;
    colA.suffix  = ":";
    colB.prefixOutside = ' ';

    // Rank / XP
    const int xpNow     = user->computeXp();
    const int rankNow   = user->computeRank();
    const int xpThisTier = User::xpForRank( rankNow );
    const int xpNextTier = (rankNow < User::MAX_RANK)
        ? User::xpForRank( rankNow + 1 )
        : xpThisTier;
    char xpProgress[64];
    if (rankNow < User::MAX_RANK) {
        Com_sprintf( xpProgress, sizeof(xpProgress),
            "%d  (%d / %d to next)", xpNow, xpNow - xpThisTier, xpNextTier - xpThisTier );
    } else {
        Com_sprintf( xpProgress, sizeof(xpProgress), "%d  (max rank)", xpNow );
    }

    const int repPos = user->statRepPositive;
    const int repNeg = user->statRepNegative;
    char repBuf[32];
    Com_sprintf( repBuf, sizeof(repBuf), "+%d / -%d  (net %+d)",
        repPos, repNeg, repPos - repNeg );

    Buffer buf;
    buf << xheader( "-PLAYER PROFILE" )
        << '\n' << colA("player")        << xvalue( displayName );

    if (!user->customClanTag.empty())
        buf << '\n' << colA("clan tag")      << xvalue( user->customClanTag );
    if (!user->customTitle.empty())
        buf << '\n' << colA("title")         << xvalue( user->customTitle );

    buf << '\n' << colA("admin level")   << colB( user->authLevel )
        << '\n' << colA("rank")          << colB( rankNow ) << " / " << xvalue( (int)User::MAX_RANK )
        << '\n' << colA("xp")            << colB( xpProgress )
        << '\n' << colA("reputation")    << colB( repBuf )
        << '\n' << colA("first seen")    << colB( firstSeenBuf )
        << '\n' << colA("playtime")      << colB( playtimeBuf )
        << '\n' << xheader( "-COMBAT" )
        << '\n' << colA("kills")         << colB( kills )
        << '\n' << colA("deaths")        << colB( deaths )
        << '\n' << colA("K/D ratio")     << colB( kdBuf )
        << '\n' << colA("headshots")     << colB( hsHits )
                                         << " (" << xvalue( hsPctBuf ) << "%)"
        << '\n' << colA("accuracy")      << colB( accPctBuf ) << "%"
                                         << "  (" << xvalue( shotsH ) << "/"
                                         << xvalue( shotsF ) << ")"
        << '\n' << colA("longest streak")<< colB( streak )
        << '\n' << xheader( "-SKILL RATING" )
        << '\n' << colA("rating")        << colB( rating ) << " / 1000";

    if (kills < 25) {
        buf << '\n' << "  (provisional — under 25 lifetime kills)";
    }

    buf << '\n' << xheader( "-ACHIEVEMENTS" )
        << '\n' << colA("unlocked")
        << colB( user->achievementsUnlocked() )
        << " / " << xvalue( (int)Ach::NUM );

    if (!user->customSignature.empty()) {
        buf << '\n' << xheader( "-SIGNATURE" )
            << '\n' << xvalue( user->customSignature );
    }

    print( txt._client, buf );
    return PA_NONE;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace cmd
