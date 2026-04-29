#include <bgame/impl.h>

namespace cmd {

///////////////////////////////////////////////////////////////////////////////

Card::Card()
    : AbstractBuiltin( "card", true /* grantAlways */ )
{
    __usage << xvalue( "!" + _name ) << ' ' << _ovalue( "PLAYER" );
    __descr << "Compact one-card profile summary.";
}

///////////////////////////////////////////////////////////////////////////////

Card::~Card()
{
}

///////////////////////////////////////////////////////////////////////////////

AbstractCommand::PostAction
Card::doExecute( Context& txt )
{
    if (txt._args.size() > 2)
        return PA_USAGE;

    // Resolve user — same precedence as !profile.
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
        Client* target = NULL;
        if (!lookupPLAYER( txt._args[1], txt, target )) {
            user = connectedUsers[target->slot];
            displayName = user ? user->namex : target->gclient.pers.netname;
        }
        else {
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
            user = *users.begin();
            displayName = user->namex;
        }
    }

    if (!user || user->isNull()) {
        txt._ebuf << "No profile data available.";
        return PA_ERROR;
    }

    const int   kills    = user->statKills;
    const int   deaths   = user->statDeaths;
    const float kd       = (deaths > 0) ? (float)kills / (float)deaths : (float)kills;
    const float acc      = user->computeAccuracy();
    const int   rating   = user->computeSkillRating();
    const int   rank     = user->computeRank();
    const int   achCnt   = user->achievementsUnlocked();
    const int   netRep   = user->statRepPositive - user->statRepNegative;

    char kdBuf[16];   Com_sprintf( kdBuf,  sizeof(kdBuf),  "%.2f", kd );
    char accBuf[16];  Com_sprintf( accBuf, sizeof(accBuf), "%.1f%%", acc * 100.f );

    // Build optional clan-tag prefix and title suffix.
    string nameLine;
    if (!user->customClanTag.empty()) {
        nameLine += "[";
        nameLine += user->customClanTag;
        nameLine += "] ";
    }
    nameLine += displayName;
    if (!user->customTitle.empty()) {
        nameLine += "  ^7- ^3";
        nameLine += user->customTitle;
    }

    // Compact 3-line card.  Uses xvalue colour by default (tilted yellow);
    // labels in default white.
    Buffer buf;
    buf << xheader( "-PLAYER CARD" )
        << '\n' << xvalue( nameLine )
        << "  ^5Rank " << xvalue( rank )
        << "^7  rating " << xvalue( rating ) << "/1000"
        << '\n' << "K/D "       << xvalue( kdBuf )
        << "  acc "             << xvalue( accBuf )
        << "  kills "           << xvalue( kills )
        << "  HS "              << xvalue( user->statHeadshots )
        << '\n' << "achievements " << xvalue( achCnt ) << "/" << xvalue( (int)Ach::NUM )
        << "  rep "                << xvalue( netRep )
        << "  best streak "        << xvalue( user->statLongestStreak );

    print( txt._client, buf );
    return PA_NONE;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace cmd
