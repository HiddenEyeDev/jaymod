// Jaymod-AC: server-side anti-cheat.
//
// Protocol (text-based, runs over the existing server/client command channel):
//
//   Server -> Client : "ac_q <txn> <type> <args...>"
//   Client -> Server : "ac_r <txn> <result>"
//
// Where:
//   <txn>   monotonic per-client transaction id (uint)
//   <type>  one of: fhash, cvar, modules, mempat
//   <args>  type-specific, no embedded spaces unless quoted at protocol layer
//   <result> opaque string the detector emits; semantics per <type>
//
// The server keeps a queue of in-flight queries per client.  Responses that
// don't match a known txn id are dropped.  Queries that time out without
// a response constitute a soft violation (configurable via g_acStrict).
//
// All public symbols are prefixed `AC_` per project convention.

#ifndef G_ANTICHEAT_H
#define G_ANTICHEAT_H

namespace AC {

///////////////////////////////////////////////////////////////////////////////
// Query types (wire string in comment).

enum QueryType {
    Q_FHASH        = 0,  // "fhash"   args: <path>      result: CRC32 hex
    Q_CVAR         = 1,  // "cvar"    args: <cvarname>  result: <value>
    Q_MODULES      = 2,  // "modules" args: (none)      result: count:hash
    Q_MEMPAT       = 3,  // "mempat"  args: <sig-id>    result: hit|miss
    Q_PAKS         = 4,  // "paks"    args: (none)      result: pak list
    Q_MODULES_LIST = 5,  // "modlist" args: (none)      result: module list

    Q_NUM
};

///////////////////////////////////////////////////////////////////////////////
// Violation severities.

enum Severity {
    SEV_INFO   = 0,  // log only
    SEV_SOFT   = 1,  // warn admins (cpm to admins), accumulate score
    SEV_HARD   = 2,  // immediate ban via User DB (g_acStrict required)
};

///////////////////////////////////////////////////////////////////////////////
// Lifecycle hooks.

void onInit();                              // G_InitGame
void onShutdown();                          // G_ShutdownGame
void onClientConnect    ( int clientNum );  // after firstTime block
void onClientDisconnect ( int clientNum );  // ClientDisconnect
void onTick             ();                 // per G_RunFrame; cheap

// Called by ClientCommand("ac_r"). Returns true if it consumed the command.
bool onClientCommand    ( int clientNum );

// Manually trigger a violation (used by ban-on-soft-failure path or by
// other code that detects something AC-relevant via a different channel).
void recordViolation    ( int clientNum, const char* reason, Severity sev );

// Schedule a one-shot inspect query against `targetSlot`.  Result is routed
// to `requesterSlot` via cpm — does NOT trigger violations even on weird
// replies (this is investigation, not enforcement).  Used by !inspect.
// Returns the txn id, or 0 if the queue is full or the slot isn't connected.
int scheduleInspectQuery( int targetSlot, QueryType type, int requesterSlot );

///////////////////////////////////////////////////////////////////////////////
// Test/expectation registration.  Hand-rolled here for built-in defaults;
// admins extend via the `ac_files.cfg` / `ac_cvars.cfg` files.

// Register a FORBIDDEN CRC32 (as 8-hex-char string) for a virtual path.
// At connect time the server asks the client to hash the file; if the
// client's hash matches ANY registered forbidden hash for that path, the
// recorded severity fires.  Multiple forbidden hashes per path are
// supported — useful for blocking several known cheat variants of the same
// file.  Files not on the list pass through silently.
void registerForbiddenFileHash( const char* path, const char* hexCrc32,
                                Severity sev );

// Cvar rule kinds — see registerCvarRule.
enum CvarRuleKind {
    CVAR_EQ        = 0,  // violate when cvar value equals `a` (string compare)
    CVAR_NE        = 1,  // violate when cvar value does NOT equal `a`
    CVAR_RANGE     = 2,  // violate when numeric value outside [a, b]
    CVAR_NOTRANGE  = 3,  // violate when numeric value inside  [a, b]
};

// Register a rule the AC system applies to a cvar's value reported by the
// client.  `a` and `b` are strings; the rule kind interprets them (numeric
// or string).  `a` and `b` may be NULL when not used by the kind.
void registerCvarRule( const char* cvarName, CvarRuleKind kind,
                       const char* a, const char* b, Severity sev );

// Legacy convenience — equivalent to registerCvarRule(name, CVAR_EQ, value, NULL, sev).
void registerForbiddenCvarValue( const char* cvarName, const char* value,
                                 Severity sev );

// Load `ac_cvars.cfg` from the engine VFS.  Format documented in the file's
// header.  Returns the number of rules loaded; 0 if file missing or empty.
int loadCvarRulePolicy( const char* configPath );

// Register a memory pattern signature ID to scan for on each client.
// The actual byte pattern is baked into cg_anticheat.cpp's signature table;
// here we only carry the ID and the severity to apply on a "hit" reply.
void registerMemoryPatternSig( const char* sigId, Severity sev );

// Load `ac_files.cfg` from the engine VFS.  Each non-empty, non-comment
// line is `path|crc32hex|severity` (severity: INFO|SOFT|HARD).  Comments
// start with '#'.  Returns the number of entries loaded; 0 if the file
// is missing or empty.
int loadFileHashPolicy( const char* configPath );

///////////////////////////////////////////////////////////////////////////////

} // namespace AC

#endif // G_ANTICHEAT_H
