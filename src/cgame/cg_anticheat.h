// Jaymod-AC: client-side anti-cheat (Phase 1).
//
// Owns the on-wire response to "ac_q" server commands.  Detectors live in
// the .cpp; the only public surface is `handleQuery` (dispatched from
// CG_ServerCommand) and lifecycle hooks.

#ifndef CG_ANTICHEAT_H
#define CG_ANTICHEAT_H

namespace AC {

///////////////////////////////////////////////////////////////////////////////

void onInit();
void onShutdown();

// Called from CG_ServerCommand when CG_Argv(0) == "ac_q".  Reads the rest of
// the argv list itself.
void handleQuery();

///////////////////////////////////////////////////////////////////////////////

} // namespace AC

#endif // CG_ANTICHEAT_H
