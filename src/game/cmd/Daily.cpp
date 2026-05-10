#include <bgame/impl.h>

namespace cmd {

///////////////////////////////////////////////////////////////////////////////

Daily::Daily()
    : AbstractBuiltin( "daily", true /* grantAlways */ )
{
    __usage << xvalue( "!" + _name ) << ' ' << _ovalue( "PLAYER" );
    __descr << "Show today's rotating daily challenges and your progress on each.";
}

///////////////////////////////////////////////////////////////////////////////

Daily::~Daily()
{
}

///////////////////////////////////////////////////////////////////////////////

AbstractCommand::PostAction
Daily::doExecute( Context& txt )
{
    if (txt._args.size() > 2)
        return PA_USAGE;

    // Resolve target user — same precedence as !profile.
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

    // Pull today's three active picks.
    int active[3];
    ::Daily::getActive( active );

    // Treat stored progress as zero if the user hasn't synced today yet
    // (rotation hasn't fired for them); displays a clean state for users
    // who haven't logged in yet today.
    const bool stale = (user->dailyDayIndex != ::Daily::dayIndex());

    Buffer buf;
    buf << xheader( "-DAILY CHALLENGES" )
        << '\n' << xvalue( displayName );

    InlineText colTitle;
    colTitle.flags |= ios::left;
    colTitle.width = 18;

    for (int i = 0; i < 3; ++i) {
        const int id = active[i];
        if (id < 0) continue;

        const ::Daily::Def& d = ::Daily::kDefs[id];

        const int prog = stale ? 0 : user->dailyProgress[id];
        const bool done = !stale && (user->dailyCompleted & (1 << id));

        char statusBuf[16];
        if (done) Q_strncpyz( statusBuf, "[done]", sizeof(statusBuf) );
        else      Com_sprintf( statusBuf, sizeof(statusBuf), "[%d/%d]", prog, d.threshold );

        const char* color = done ? "^2" : (prog > 0 ? "^3" : "^8");

        buf << '\n' << color << statusBuf << "^7 "
            << colTitle( d.title ) << "^7" << d.descr
            << "  ^8(+" << d.xpReward << " XP)^7";
    }

    print( txt._client, buf );
    return PA_NONE;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace cmd
