#include <bgame/impl.h>
#include <algorithm>

namespace cmd {

namespace {

///////////////////////////////////////////////////////////////////////////////
// Sort criteria

enum Criterion {
    BY_SKILL,
    BY_KILLS,
    BY_KD,
    BY_HEADSHOTS,
    BY_ACCURACY,
    BY_PLAYTIME,
    BY_STREAK,
};

struct Row {
    const User* user;
    float       value;   // criterion-specific score we sort on
    int         rating;  // for the "skill" column shown alongside any view
};

bool rowGreater( const Row& a, const Row& b ) {
    return a.value > b.value;
}

float kd( const User& u ) {
    return (u.statDeaths > 0)
        ? (float)u.statKills / (float)u.statDeaths
        : (float)u.statKills;
}

// Player must have at least this many lifetime kills to appear on a
// leaderboard.  Filters out brand-new accounts that would otherwise top
// charts on lucky single-game numbers.
const int LB_MIN_KILLS = 25;

const int LB_LIMIT = 10;  // top-N rows printed

} // namespace

///////////////////////////////////////////////////////////////////////////////

Leaderboard::Leaderboard()
    : AbstractBuiltin( "leaderboard", true /* grantAlways */ )
{
    __usage << xvalue( "!" + _name ) << ' '
            << _ovalue( "skill|kills|kd|hs|acc|playtime|streak" );
    __descr << "Show the top players by the chosen metric (default: skill).";
}

///////////////////////////////////////////////////////////////////////////////

Leaderboard::~Leaderboard()
{
}

///////////////////////////////////////////////////////////////////////////////

AbstractCommand::PostAction
Leaderboard::doExecute( Context& txt )
{
    if (txt._args.size() > 2)
        return PA_USAGE;

    Criterion crit = BY_SKILL;
    const char* title = "TOP BY SKILL RATING";
    const char* unitHdr = "rating";

    if (txt._args.size() == 2) {
        string s = txt._args[1];
        str::toLower( s );

        if      (s == "skill")    { crit = BY_SKILL;     title = "TOP BY SKILL RATING"; unitHdr = "rating"; }
        else if (s == "kills")    { crit = BY_KILLS;     title = "TOP BY KILLS";        unitHdr = "kills";  }
        else if (s == "kd")       { crit = BY_KD;        title = "TOP BY K/D RATIO";    unitHdr = "K/D";    }
        else if (s == "hs"
              || s == "headshots"){ crit = BY_HEADSHOTS; title = "TOP BY HEADSHOTS";    unitHdr = "HS";     }
        else if (s == "acc"
              || s == "accuracy") { crit = BY_ACCURACY;  title = "TOP BY ACCURACY";     unitHdr = "acc%";   }
        else if (s == "playtime"
              || s == "time")     { crit = BY_PLAYTIME;  title = "TOP BY PLAYTIME";     unitHdr = "time";   }
        else if (s == "streak")   { crit = BY_STREAK;    title = "TOP BY KILL STREAK";  unitHdr = "streak"; }
        else                      { return PA_USAGE; }
    }

    // Collect candidate rows from the User DB.
    vector<Row> rows;
    rows.reserve( 64 );

    const UserDB::mapGUID_t::const_iterator end = userDB.mapGUID.end();
    for (UserDB::mapGUID_t::const_iterator it = userDB.mapGUID.begin(); it != end; ++it) {
        const User& u = it->second;
        if (u.fakeguid)              continue;
        if (u.statKills < LB_MIN_KILLS) continue;

        Row r;
        r.user   = &u;
        r.rating = u.computeSkillRating();

        switch (crit) {
            case BY_SKILL:     r.value = (float)r.rating;            break;
            case BY_KILLS:     r.value = (float)u.statKills;         break;
            case BY_KD:        r.value = kd(u);                      break;
            case BY_HEADSHOTS: r.value = (float)u.statHeadshots;     break;
            case BY_ACCURACY:  r.value = u.computeAccuracy();        break;
            case BY_PLAYTIME:  r.value = (float)u.statPlaytimeSecs;  break;
            case BY_STREAK:    r.value = (float)u.statLongestStreak; break;
        }
        rows.push_back( r );
    }

    if (rows.empty()) {
        txt._ebuf << "No qualifying players yet (need "
                  << xvalue( LB_MIN_KILLS ) << "+ lifetime kills).";
        return PA_ERROR;
    }

    std::sort( rows.begin(), rows.end(), rowGreater );
    if ((int)rows.size() > LB_LIMIT)
        rows.resize( LB_LIMIT );

    // Format output
    InlineText colRank;
    colRank.flags |= ios::right;
    colRank.width = 3;
    colRank.suffix = ".";

    InlineText colName;
    colName.flags |= ios::left;
    colName.width = 24;

    InlineText colVal = xvalue;
    colVal.flags |= ios::right;
    colVal.width = 9;

    InlineText colSide = xvalue;

    Buffer buf;
    char title2[96];
    Com_sprintf( title2, sizeof(title2), "-%s", title );
    buf << xheader( title2 );

    int rank = 1;
    for (vector<Row>::const_iterator it = rows.begin(); it != rows.end(); ++it, ++rank) {
        const User& u = *it->user;

        // Per-criterion value formatting
        char valBuf[32];
        switch (crit) {
            case BY_SKILL:
            case BY_KILLS:
            case BY_HEADSHOTS:
            case BY_STREAK:
                Com_sprintf( valBuf, sizeof(valBuf), "%d", (int)it->value );
                break;

            case BY_KD:
                Com_sprintf( valBuf, sizeof(valBuf), "%.2f", it->value );
                break;

            case BY_ACCURACY:
                Com_sprintf( valBuf, sizeof(valBuf), "%.1f%%", it->value * 100.f );
                break;

            case BY_PLAYTIME: {
                int s = (int)it->value;
                const int d = s / 86400; s %= 86400;
                const int h = s / 3600;  s %= 3600;
                const int m = s / 60;
                if (d > 0)      Com_sprintf( valBuf, sizeof(valBuf), "%dd%dh%dm", d, h, m );
                else if (h > 0) Com_sprintf( valBuf, sizeof(valBuf), "%dh%dm", h, m );
                else            Com_sprintf( valBuf, sizeof(valBuf), "%dm", m );
                break;
            }
        }

        buf << '\n' << colRank( rank ) << ' '
            << colName( u.namex )
            << colVal( valBuf );

        // Always show skill rating alongside non-skill views, for context.
        if (crit != BY_SKILL) {
            buf << "  (" << colSide( it->rating ) << " skill)";
        }
    }

    buf << '\n';

    print( txt._client, buf );
    return PA_NONE;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace cmd
