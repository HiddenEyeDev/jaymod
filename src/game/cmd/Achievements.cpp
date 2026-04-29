#include <bgame/impl.h>

namespace cmd {

///////////////////////////////////////////////////////////////////////////////

Achievements::Achievements()
    : AbstractBuiltin( "achievements", true /* grantAlways */ )
{
    __usage << xvalue( "!" + _name ) << ' ' << _ovalue( "PLAYER" );
    __descr << "List the player's achievements with unlock status.";
}

///////////////////////////////////////////////////////////////////////////////

Achievements::~Achievements()
{
}

///////////////////////////////////////////////////////////////////////////////

AbstractCommand::PostAction
Achievements::doExecute( Context& txt )
{
    if (txt._args.size() > 2)
        return PA_USAGE;

    // Resolve target user (same logic as !profile: self -> online -> DB).
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

    // Build header
    Buffer buf;
    char hdr[96];
    Com_sprintf( hdr, sizeof(hdr), "-ACHIEVEMENTS (%d / %d)",
        user->achievementsUnlocked(), (int)Ach::NUM );
    buf << xheader( hdr )
        << '\n' << xvalue( displayName );

    // Layout: status column + title column + description column.
    InlineText colStatus;
    colStatus.flags |= ios::right;
    colStatus.width = 6;

    InlineText colTitle;
    colTitle.flags |= ios::left;
    colTitle.width = 18;

    for (int i = 0; i < (int)Ach::NUM; i++) {
        const Ach::Def& def = Ach::kDefs[i];
        const int       n   = user->achCount[i];

        // Status cell:
        //   "[x N]"  for repeatable with N earnings
        //   "[ +1 ]"  for unlocked non-repeatable
        //   "[ ?? ]"  for locked
        char statusBuf[12];
        if (n <= 0) {
            Q_strncpyz( statusBuf, "[  ]", sizeof(statusBuf) );
        } else if (def.repeatable) {
            Com_sprintf( statusBuf, sizeof(statusBuf), "[%dx]", n );
        } else {
            Q_strncpyz( statusBuf, "[ x]", sizeof(statusBuf) );
        }

        // Color the title differently when locked vs unlocked so the eye
        // can scan a long list quickly.  Use ^5 (cyan) when unlocked, ^8
        // (grey) when locked.
        const char* titleColor = (n > 0) ? "^5" : "^8";

        buf << '\n' << colStatus( statusBuf ) << ' '
            << titleColor << colTitle( def.title )
            << "^7" << def.descr;
    }

    print( txt._client, buf );
    return PA_NONE;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace cmd
