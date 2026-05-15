#include <bgame/impl.h>

namespace cmd {

///////////////////////////////////////////////////////////////////////////////

Inspect::Inspect()
    : AbstractBuiltin( "inspect" )   // NOT grantAlways — admin level required
{
    __usage << xvalue( "!" + _name ) << ' ' << xvalue( "PLAYER" );
    __descr << "Ask the AC system to report a player's loaded pk3 files and "
            << "process modules.  Results stream back to you via cpm over "
            << "the next few seconds.";
}

///////////////////////////////////////////////////////////////////////////////

Inspect::~Inspect()
{
}

///////////////////////////////////////////////////////////////////////////////

AbstractCommand::PostAction
Inspect::doExecute( Context& txt )
{
    if (txt._args.size() != 2)
        return PA_USAGE;

    Client* target = NULL;
    if (lookupPLAYER( txt._args[1], txt, target ))
        return PA_ERROR;

    // Issue both queries.  Results route back to the calling admin via the
    // requester field; non-admin calls already get rejected at command
    // dispatch (this command is not grantAlways).
    const int requester = txt._client ? txt._client->slot : -1;

    const int txnPaks = AC::scheduleInspectQuery( target->slot, AC::Q_PAKS,         requester );
    const int txnMods = AC::scheduleInspectQuery( target->slot, AC::Q_MODULES_LIST, requester );

    if (txnPaks == 0 && txnMods == 0) {
        txt._ebuf << "AC queue is full or target isn't tracked.";
        return PA_ERROR;
    }

    Buffer buf;
    buf << "Inspecting " << xvalue( target->gclient.pers.netname )
        << ".  Results will be appended to "
        << xvalue( "<mod>/inspections/<name>_guid<guid>.log" )
        << " over the next few seconds.";
    print( txt._client, buf );

    return PA_NONE;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace cmd
