#include <bgame/impl.h>

namespace cmd {

///////////////////////////////////////////////////////////////////////////////

Rep::Rep()
    : AbstractBuiltin( "rep", true /* grantAlways: anyone can rep */ )
{
    __usage << xvalue( "!" + _name ) << ' '
            << xvalue( "+|-" ) << ' '
            << xvalue( "PLAYER" );
    __descr << "Give a player +1 or -1 community reputation.  Cooldown applies.";
}

///////////////////////////////////////////////////////////////////////////////

Rep::~Rep()
{
}

///////////////////////////////////////////////////////////////////////////////

AbstractCommand::PostAction
Rep::doExecute( Context& txt )
{
    if (txt._args.size() != 3)
        return PA_USAGE;

    // Console can't give rep — there's no giver identity to throttle on.
    if (!txt._client) {
        txt._ebuf << "Console cannot give reputation.";
        return PA_ERROR;
    }

    // Parse direction
    string dir = txt._args[1];
    int delta = 0;
    if      (dir == "+" || dir == "up"   || dir == "+1") delta = +1;
    else if (dir == "-" || dir == "down" || dir == "-1") delta = -1;
    else                                                  return PA_USAGE;

    // Cooldown — per giver, configurable via g_repCooldown (seconds).
    const time_t now      = time( NULL );
    const time_t cdUntil  = txt._client->gclient.pers.repCooldownUntil;
    if (cdUntil > now) {
        const string left = str::toStringSecondsRemaining( cdUntil - now, true );
        txt._ebuf << "You must wait " << xvalue( left ) << " before giving rep again.";
        return PA_ERROR;
    }

    // Resolve target — online lookup first, fall back to DB by name.
    User* target = NULL;
    string targetName;
    Client* online = NULL;
    if (!lookupPLAYER( txt._args[2], txt, online )) {
        target     = connectedUsers[online->slot];
        targetName = target ? target->namex : online->gclient.pers.netname;
    }
    else {
        txt._ebuf.reset();
        const string name = SanitizeString( txt._args[2], false );
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
        target     = *users.begin();
        targetName = target->namex;
    }

    if (!target || target->isNull() || target->fakeguid) {
        txt._ebuf << "That player has no profile to rep.";
        return PA_ERROR;
    }

    const User* self = connectedUsers[txt._client->slot];
    if (target == self) {
        txt._ebuf << "You cannot rep yourself.";
        return PA_ERROR;
    }

    // Apply
    if (delta > 0) target->statRepPositive++;
    else           target->statRepNegative++;

    int cd = g_repCooldown.integer;
    if (cd < 1) cd = 300;
    txt._client->gclient.pers.repCooldownUntil = now + cd;

    // Broadcast — public so the recipient sees it and the community can
    // sanity-check who's been spamming negs.
    const char* giver = txt._client->gclient.pers.netname;
    if (delta > 0) {
        AP( va( "chat \"^3*** ^7%s ^7gave ^2+1 rep ^7to ^5%s\"",
            giver, targetName.c_str() ) );
    } else {
        AP( va( "chat \"^3*** ^7%s ^7gave ^1-1 rep ^7to ^5%s\"",
            giver, targetName.c_str() ) );
    }

    return PA_NONE;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace cmd
