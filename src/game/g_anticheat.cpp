// Jaymod-AC: server-side anti-cheat (Phase 1).
//
// Implements the protocol scaffolding and two basic detectors (file hash
// and cvar audit) with a local-ban + master-server-stub violation path.
// See g_anticheat.h for the wire protocol.

#include <bgame/impl.h>
#include "g_anticheat.h"

namespace AC {

///////////////////////////////////////////////////////////////////////////////

namespace {

// ---- Configuration ---------------------------------------------------------

const int kMaxPendingPerClient = 8;       // queue depth before drops
const int kDefaultTimeoutMs    = 30000;   // 30s response window
const int kInitialDelayMs      = 8000;    // wait 8s after connect before first
                                          // query (let the client finish loading)
const int kTickThrottleMs      = 250;     // onTick budget — only act this often
const int kSendSpacingMs       = 1100;    // min gap between consecutive ac_q
                                          // sends to one client.  sv_floodProtect
                                          // on the server side silently drops
                                          // client commands that arrive too
                                          // fast, so bursting 7 ac_q to a
                                          // client and getting 7 ac_r back in
                                          // the same frame loses all but the
                                          // first.  1100ms beats the default
                                          // ~1s threshold comfortably.
const int kMaxOutboundQueue    = 32;      // bound the per-client backlog

// ---- Pending-query state ---------------------------------------------------

struct Pending {
    bool       active;
    int        txn;          // transaction id, 0 = slot free
    QueryType  type;
    int        deadlineMs;   // level.time at which we give up
    string     args;         // wire args (e.g. "etconfig.cfg")
    string     expected;     // optional expected result (for fhash / cvar)
    Severity   onMismatch;   // severity if expected != actual
    int        requester;    // admin slot to receive the result, or -1
                             // when this is a normal AC scan (no routing).
};

// Buffered query waiting to be sent.  We don't allocate a Pending slot until
// the query actually goes on the wire, so the queue and the timeout window
// stay decoupled — a query that sits in the queue for 5 seconds still gets
// 30 seconds to receive a response once it ships.
struct OutboundItem {
    QueryType type;
    string    args;
    string    expected;
    Severity  sev;
    int       requester;
};

// ---- Per-client state ------------------------------------------------------

struct ClientState {
    bool                  connected;
    int                   nextTxn;
    int                   handshakeFireAt;  // level.time at which we enqueue the first batch
    bool                  handshakeSent;
    int                   violationsScore;  // accumulator; high = boot
    Pending               pending[kMaxPendingPerClient];

    // Staggered outbound queue.  scheduleQueryInternal pushes here; the
    // drain in onTick sends one entry per kSendSpacingMs to avoid tripping
    // sv_floodProtect on the responses coming back.
    vector<OutboundItem>  outboundQueue;
    int                   nextSendAt;       // level.time at which next drain may fire
};

ClientState g_clients[ MAX_CLIENTS ];

// ---- Expected-value registry ----------------------------------------------
//
// Phase 1: hand-registered at AC::onInit().  Phase 3 will load from a config
// file or be driven by master-server policy.

struct ForbiddenHash {
    string   path;
    string   hexCrc32;   // lowercase 8-char hex — match THIS = violation
    Severity sev;
};

struct CvarRule {
    string       name;
    CvarRuleKind kind;
    string       a;      // first arg (or value)
    string       b;      // second arg (range upper bound)
    Severity     sev;
};

struct MemPatSig {
    string   id;      // wire identifier — client looks it up in its sig table
    Severity sev;     // severity on "hit"
};

vector<ForbiddenHash>  g_forbiddenHashes;
vector<CvarRule>       g_cvarRules;
vector<MemPatSig>      g_memPatSigs;

int g_lastTickAt = 0;

// ---- Helpers ---------------------------------------------------------------

User* resolveUser( int clientNum ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return NULL;
    User* user = connectedUsers[clientNum];
    if (!user || user->isNull() || user->fakeguid) return NULL;
    const gentity_t* ent = &g_entities[clientNum];
    if (ent->r.svFlags & SVF_BOT) return NULL;
    return user;
}

const char* netname( int clientNum ) {
    const gentity_t* ent = &g_entities[clientNum];
    return (ent && ent->client) ? ent->client->pers.netname : "?";
}

// Replace any byte >= 0x80 (signed-char < 0) with '?'.  The engine's
// trap_SendServerCommand filter drops the whole message if it finds any
// high-bit byte, so anything we forward from user-controlled sources
// (cvar values, module names, file paths) must be sanitised first.
void sanitizeAscii( string& s ) {
    for (size_t i = 0; i < s.length(); ++i) {
        if ((signed char)s[i] < 0) s[i] = '?';
    }
}

// Returns a sanitized copy suitable for direct embedding in cpm/chat payloads.
string asciiSafe( const char* s ) {
    string out = s ? s : "";
    sanitizeAscii( out );
    return out;
}

// Pretty-print helpers for the inspections log.
//
// formatPaksList: parses "dir/name:bytes dir/name:bytes ..." tokens and
// emits one line per pak with a 48-column path and a human-readable size.
// Tracks a trailing "...(N more)" truncation marker from the client and
// preserves it on its own line.  Returns the number of paks rendered.
int formatPaksList( const string& raw, string& out ) {
    size_t i = 0;
    int    count    = 0;
    string trailing;

    while (i < raw.length()) {
        while (i < raw.length() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
        if (i >= raw.length()) break;

        // If we hit the "..." truncation marker, capture everything to EOL.
        if (raw[i] == '.' && i + 2 < raw.length() && raw[i+1] == '.' && raw[i+2] == '.') {
            trailing = raw.substr( i );
            break;
        }

        const size_t tokenStart = i;
        while (i < raw.length() && raw[i] != ' ' && raw[i] != '\t') ++i;
        const string token = raw.substr( tokenStart, i - tokenStart );

        // Split on the LAST ':' so paths containing colons don't confuse us.
        const size_t colon   = token.rfind( ':' );
        const string name    = (colon != string::npos) ? token.substr( 0, colon ) : token;
        const string sizeStr = (colon != string::npos) ? token.substr( colon + 1 ) : "";

        // Human-readable size.
        char human[32];
        const long long bytes = atoll( sizeStr.c_str() );
        if      (bytes >= 1024LL * 1024LL * 1024LL)
            Com_sprintf( human, sizeof(human), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0) );
        else if (bytes >= 1024LL * 1024LL)
            Com_sprintf( human, sizeof(human), "%.1f MB", bytes / (1024.0 * 1024.0) );
        else if (bytes >= 1024LL)
            Com_sprintf( human, sizeof(human), "%.1f KB", bytes / 1024.0 );
        else if (bytes > 0)
            Com_sprintf( human, sizeof(human), "%lld bytes", bytes );
        else
            Q_strncpyz( human, "?", sizeof(human) );

        char line[256];
        Com_sprintf( line, sizeof(line), "  %-48s  %s\n", name.c_str(), human );
        out += line;
        ++count;
    }

    if (!trailing.empty()) {
        out += "  ";
        out += trailing;
        out += "\n";
    }
    return count;
}

// formatModulesList: emits one module name per line.  Same truncation-marker
// handling as paks.
int formatModulesList( const string& raw, string& out ) {
    size_t i = 0;
    int    count = 0;
    string trailing;

    while (i < raw.length()) {
        while (i < raw.length() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
        if (i >= raw.length()) break;

        if (raw[i] == '.' && i + 2 < raw.length() && raw[i+1] == '.' && raw[i+2] == '.') {
            trailing = raw.substr( i );
            break;
        }

        const size_t tokenStart = i;
        while (i < raw.length() && raw[i] != ' ' && raw[i] != '\t') ++i;
        const string token = raw.substr( tokenStart, i - tokenStart );

        out += "  ";
        out += token;
        out += "\n";
        ++count;
    }

    if (!trailing.empty()) {
        out += "  ";
        out += trailing;
        out += "\n";
    }
    return count;
}

const char* queryTypeName( QueryType t ) {
    switch (t) {
        case Q_FHASH:        return "fhash";
        case Q_CVAR:         return "cvar";
        case Q_MODULES:      return "modules";
        case Q_MEMPAT:       return "mempat";
        case Q_PAKS:         return "paks";
        case Q_MODULES_LIST: return "modlist";
        default:             return "?";
    }
}

Pending* findPending( ClientState& cs, int txn ) {
    if (txn <= 0) return NULL;
    for (int i = 0; i < kMaxPendingPerClient; ++i) {
        if (cs.pending[i].active && cs.pending[i].txn == txn)
            return &cs.pending[i];
    }
    return NULL;
}

Pending* allocPending( ClientState& cs ) {
    for (int i = 0; i < kMaxPendingPerClient; ++i) {
        if (!cs.pending[i].active) {
            cs.pending[i].active = true;
            return &cs.pending[i];
        }
    }
    return NULL;  // queue full
}

void freePending( Pending& p ) {
    p.active     = false;
    p.txn        = 0;
    p.deadlineMs = 0;
    p.type       = Q_FHASH;
    p.onMismatch = SEV_INFO;
    p.args.clear();
    p.expected.clear();
    p.requester  = -1;
}

// Reset a ClientState without using memset — Pending contains std::string
// members, and memset-ing those is undefined behaviour (it shreds the
// string's internal pointer/size/capacity, leading to garbage reads later
// via .c_str() and crashes on assignment).
void resetClientState( ClientState& cs ) {
    cs.connected       = false;
    cs.nextTxn         = 0;
    cs.handshakeFireAt = 0;
    cs.handshakeSent   = false;
    cs.violationsScore = 0;
    cs.nextSendAt      = 0;
    cs.outboundQueue.clear();
    for (int i = 0; i < kMaxPendingPerClient; ++i) {
        freePending( cs.pending[i] );
    }
}

// Send a query over the existing server command channel.
//
// IMPORTANT: server commands are space-tokenised, so args must be free of
// embedded spaces.  We trust the registration code to pass sensible paths.
void sendQuery( int clientNum, Pending& p ) {
    // DEBUG: instrument outgoing query — remove once protocol is verified.
    G_LogPrintf( "AC.DEBUG: send  client=%d txn=%d type=%s args=\"%s\"\n",
        clientNum, p.txn, queryTypeName( p.type ), p.args.c_str() );

    trap_SendServerCommand( clientNum,
        va( "ac_q %d %s %s",
            p.txn, queryTypeName( p.type ), p.args.c_str() ) );
}

// Enqueue a query for staggered transmission.  Returns 1 if accepted, 0 if
// the per-client backlog is full (very rare — kMaxOutboundQueue is large
// enough for any sane handshake batch plus a few inspect requests).
int scheduleQueryInternal( int clientNum, QueryType type,
                           const string& args, const string& expected,
                           Severity onMismatch, int requester = -1 )
{
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return 0;

    ClientState& cs = g_clients[clientNum];
    if (!cs.connected) return 0;

    if ((int)cs.outboundQueue.size() >= kMaxOutboundQueue) return 0;

    OutboundItem item;
    item.type      = type;
    item.args      = args;
    item.expected  = expected;
    item.sev       = onMismatch;
    item.requester = requester;

    // If the queue was empty, the first drain would fire immediately (next
    // tick).  That's a problem when scheduleQueryInternal is being called
    // from a /say-dispatched admin command (e.g. !inspect) — sv_floodProtect
    // has just consumed a command slot for the chat itself, and the very
    // first ac_r reply will arrive milliseconds later and get silently
    // dropped before our ClientCommand handler runs.
    //
    // Push the first send one full spacing into the future so it's clear of
    // whatever command preceded it.
    const bool wasEmpty = cs.outboundQueue.empty();
    cs.outboundQueue.push_back( item );
    if (wasEmpty && cs.nextSendAt < level.time + kSendSpacingMs) {
        cs.nextSendAt = level.time + kSendSpacingMs;
    }
    return 1;
}

// Drain one queued OutboundItem if we're past the per-client rate limit.
// Allocates a Pending slot, assigns a fresh txn, and ships the ac_q.
void drainOutboundQueue( int clientNum ) {
    ClientState& cs = g_clients[clientNum];
    if (cs.outboundQueue.empty()) return;
    if (level.time < cs.nextSendAt) return;

    // Pop front.
    OutboundItem item = cs.outboundQueue.front();
    cs.outboundQueue.erase( cs.outboundQueue.begin() );

    cs.nextSendAt = level.time + kSendSpacingMs;

    Pending* p = allocPending( cs );
    if (!p) return;  // pending queue full — query is silently dropped

    cs.nextTxn++;
    if (cs.nextTxn <= 0) cs.nextTxn = 1;

    p->txn        = cs.nextTxn;
    p->type       = item.type;
    p->deadlineMs = level.time + kDefaultTimeoutMs;
    p->args       = item.args;
    p->expected   = item.expected;
    p->onMismatch = item.sev;
    p->requester  = item.requester;

    sendQuery( clientNum, *p );
}

// ---- Violation logging -----------------------------------------------------
//
// Local-only: write a structured G_LogPrintf line so admins have forensics.
// Persistent bans land in users.db via recordViolation's User-field path.

void writeViolationLog( int clientNum, const char* reason, Severity sev ) {
    const User* user = resolveUser( clientNum );
    const char* guid = user ? user->guid.c_str() : "<no-guid>";
    G_LogPrintf( "AC: VIOLATION guid=%s name=%s sev=%d reason=\"%s\"\n",
        guid, netname(clientNum), (int)sev, reason );
}

// ---- Detector evaluation ---------------------------------------------------

void evaluateResponse( int clientNum, Pending& p, const string& result ) {
    bool matched = true;

    if (!p.expected.empty()) {
        // Case-insensitive compare so hex digit case from the client doesn't
        // false-positive.  Wider matching policies (regex, ranges) come later.
        string a = p.expected; str::toLower( a );
        string b = result;     str::toLower( b );
        matched = (a == b);
    }

    if (matched) {
        G_LogPrintf( "AC: client %d (%s) %s ok: %s\n",
            clientNum, netname(clientNum),
            queryTypeName(p.type), p.args.c_str() );
        return;
    }

    // Mismatch — record a violation at the configured severity.
    char buf[256];
    Com_sprintf( buf, sizeof(buf), "%s mismatch on %s: expected=%s got=%s",
        queryTypeName(p.type), p.args.c_str(),
        p.expected.c_str(), result.c_str() );
    recordViolation( clientNum, buf, p.onMismatch );
}

// Cvars live at global scope (defined in g_main.cpp, extern in g_jaymod.h
// which is pulled in via the PCH chain).  Nothing else needed here.

} // namespace
} // namespace AC

namespace AC {

///////////////////////////////////////////////////////////////////////////////
// Public API
///////////////////////////////////////////////////////////////////////////////

void onInit() {
    for (int i = 0; i < MAX_CLIENTS; ++i) resetClientState( g_clients[i] );
    g_forbiddenHashes.clear();
    g_cvarRules.clear();
    g_memPatSigs.clear();
    g_lastTickAt = 0;

    // Minimal built-in cvar policy — the obviously-cheating defaults that
    // ship without an ac_cvars.cfg.  Admins extend / override via the config.
    registerCvarRule( "r_showtris",     CVAR_EQ, "1", NULL, SEV_HARD );
    registerCvarRule( "cg_thirdperson", CVAR_EQ, "1", NULL, SEV_HARD );

    // Built-in mempat signatures.  Names MUST match the IDs in
    // cg_anticheat.cpp's kSignatures table.  Phase 2 ships placeholders.
    registerMemoryPatternSig( "sig_test1", SEV_HARD );
    registerMemoryPatternSig( "sig_test2", SEV_HARD );

    // Cvar-rule policy from VFS.  Missing file is fine; built-ins above
    // remain active.
    {
        const int n = loadCvarRulePolicy( "ac_cvars.cfg" );
        if (n > 0) G_Printf( "AC: loaded %d cvar rule(s) from ac_cvars.cfg\n", n );
    }

    // File-hash policy from VFS.  Missing file is fine (Phase 2 default).
    {
        const int n = loadFileHashPolicy( "ac_files.cfg" );
        if (n > 0) G_Printf( "AC: loaded %d file-hash policy entries\n", n );
    }

    G_Printf( "AC: anti-cheat initialised (enabled=%d strict=%d)\n",
        g_acEnabled.integer, g_acStrict.integer );
}

void onShutdown() {
    for (int i = 0; i < MAX_CLIENTS; ++i) resetClientState( g_clients[i] );
    g_forbiddenHashes.clear();
    g_cvarRules.clear();
    g_memPatSigs.clear();
}

void onClientConnect( int clientNum ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return;

    ClientState& cs = g_clients[clientNum];
    resetClientState( cs );
    cs.connected        = true;
    cs.handshakeFireAt  = level.time + kInitialDelayMs;
}

void onClientDisconnect( int clientNum ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return;
    ClientState& cs = g_clients[clientNum];

    // Clear out any pending queries so they don't time out into violations.
    for (int i = 0; i < kMaxPendingPerClient; ++i) {
        if (cs.pending[i].active) freePending( cs.pending[i] );
    }
    cs.connected = false;
}

void onTick() {
    if (!g_acEnabled.integer) return;

    // Throttle.  G_RunFrame is called every server frame (~20Hz).  We don't
    // need to walk every client's pending queue that often.
    if (g_lastTickAt && level.time - g_lastTickAt < kTickThrottleMs) return;
    g_lastTickAt = level.time;

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        ClientState& cs = g_clients[i];
        if (!cs.connected) continue;

        // Fire the connect-time batch.
        if (!cs.handshakeSent && level.time >= cs.handshakeFireAt) {
            cs.handshakeSent = true;

            // Cvar audits.  Issue one query per unique cvar — multiple rules
            // on the same cvar are evaluated together against the single
            // response.  De-dup by walking the rules list and skipping any
            // name we've already queued this batch.
            for (size_t k = 0; k < g_cvarRules.size(); ++k) {
                const string& name = g_cvarRules[k].name;

                bool already = false;
                for (size_t j = 0; j < k; ++j) {
                    if (g_cvarRules[j].name == name) { already = true; break; }
                }
                if (already) continue;

                // expected="" means "let onClientCommand evaluate rules";
                // onMismatch is unused here (real severity comes from the
                // matching rule).
                scheduleQueryInternal( i, Q_CVAR, name, /*expected=*/"",
                    /*onMismatch=*/SEV_INFO );
            }

            // File hash checks
            // File hashes: one query per UNIQUE path; the response handler
            // walks every forbidden-hash rule for that path and fires a
            // violation on any match.  expected="" signals "evaluate in the
            // handler" — same convention as cvar audits.
            for (size_t k = 0; k < g_forbiddenHashes.size(); ++k) {
                const string& path = g_forbiddenHashes[k].path;
                bool already = false;
                for (size_t j = 0; j < k; ++j) {
                    if (g_forbiddenHashes[j].path == path) { already = true; break; }
                }
                if (already) continue;

                scheduleQueryInternal( i, Q_FHASH, path, /*expected=*/"",
                    /*onMismatch=*/SEV_INFO );
            }

            // Module-list fingerprint.  No ground-truth comparison in
            // Phase 2 — the response handler just logs what we got so
            // admins can build allowlists over time.  Pass an empty
            // expected so evaluateResponse always matches (no false-violation).
            scheduleQueryInternal( i, Q_MODULES, /*args*/ "", /*expected*/ "",
                                   /*onMismatch*/ SEV_INFO );

            // Memory pattern scans — one query per registered signature.
            // The "expected" answer is the literal string "miss"; a "hit"
            // reply triggers the violation at the registered severity.
            for (size_t k = 0; k < g_memPatSigs.size(); ++k) {
                const MemPatSig& s = g_memPatSigs[k];
                scheduleQueryInternal( i, Q_MEMPAT, s.id,
                                       /*expected*/ "miss",
                                       /*onMismatch*/ s.sev );
            }
        }

        // Drain the staggered outbound queue (at most one per kSendSpacingMs).
        drainOutboundQueue( i );

        // Time out anything past its deadline.
        for (int s = 0; s < kMaxPendingPerClient; ++s) {
            Pending& p = cs.pending[s];
            if (!p.active) continue;
            if (level.time < p.deadlineMs) continue;

            const int timeoutMs = g_acTimeout.integer > 0
                ? g_acTimeout.integer : kDefaultTimeoutMs;

            if (p.requester >= 0 && p.requester < MAX_CLIENTS) {
                // Inspect timeout — notify the admin who issued it, never
                // violation the target.  This is investigation, not
                // enforcement; missing replies can be packet drops,
                // sv_floodProtect, or a misbehaving client, none of which
                // warrant punishing the player being inspected.
                trap_SendServerCommand( p.requester,
                    va( "cpm \"^1AC inspect:^7 no reply from ^5%s^7 for %s within %dms\n\"",
                        asciiSafe( netname(i) ).c_str(),
                        queryTypeName( p.type ), timeoutMs ) );
                G_LogPrintf( "AC: inspect timeout client=%d type=%s args=\"%s\"\n",
                    i, queryTypeName( p.type ), p.args.c_str() );
            }
            else {
                // Real AC query timeout — soft violation as before.
                char buf[256];
                Com_sprintf( buf, sizeof(buf),
                    "no response to %s %s within %dms",
                    queryTypeName( p.type ), p.args.c_str(), timeoutMs );
                recordViolation( i, buf, SEV_SOFT );
            }
            freePending( p );
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Client command dispatch — invoked from ClientCommand() when argv(0)=="ac_r"

bool onClientCommand( int clientNum ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return false;
    ClientState& cs = g_clients[clientNum];
    if (!cs.connected) return true;  // we own the cmd even if state is gone

    // Format: ac_r <txn> <result...>
    char buf[MAX_TOKEN_CHARS];
    trap_Argv( 1, buf, sizeof(buf) );
    const int txn = atoi( buf );

    // Concatenate args 2..argc-1 with spaces — result strings may contain
    // tokens like file paths.  Most detector results are single tokens
    // anyway.
    const int argc = trap_Argc();
    string result;
    for (int i = 2; i < argc; ++i) {
        if (i > 2) result += ' ';
        trap_Argv( i, buf, sizeof(buf) );
        result += buf;
    }

    // Anything we relay back via cpm / chat MUST be 7-bit ASCII or the
    // engine drops the message.  Filenames, cvar values, module names —
    // all come from the client and can contain high-bit bytes.
    sanitizeAscii( result );

    // DEBUG: confirm the response actually reached the server.
    G_LogPrintf( "AC.DEBUG: recv  client=%d txn=%d result=\"%s\"\n",
        clientNum, txn, result.c_str() );

    Pending* p = findPending( cs, txn );
    if (!p) {
        // Late reply to a timed-out query, or txn we never sent.  Log it
        // (could indicate a probing/replay attempt) but don't violation:
        // could equally be a slow legit client.
        G_LogPrintf( "AC: client %d unknown txn %d\n", clientNum, txn );
        return true;
    }

    // Cvar audit: run every registered rule that applies to this cvar
    // against the single response.  Each matched rule produces a violation.
    if (p->type == Q_CVAR && p->expected.empty()) {
        const string& name = p->args;

        for (size_t k = 0; k < g_cvarRules.size(); ++k) {
            const CvarRule& r = g_cvarRules[k];
            if (Q_stricmp( r.name.c_str(), name.c_str() ) != 0) continue;

            bool   violated = false;
            string detail;  // human-readable description for the log line

            switch (r.kind) {
                case CVAR_EQ: {
                    if (Q_stricmp( result.c_str(), r.a.c_str() ) == 0) {
                        violated = true;
                        detail = "==";
                    }
                    break;
                }
                case CVAR_NE: {
                    if (Q_stricmp( result.c_str(), r.a.c_str() ) != 0) {
                        violated = true;
                        detail = "!=";
                    }
                    break;
                }
                case CVAR_RANGE: {
                    // Numeric. atof returns 0 on garbage; for cvars that aren't
                    // numeric this rule shouldn't be used, but guard anyway:
                    // if result is non-numeric (no digits at all), skip.
                    const char* rs = result.c_str();
                    bool numeric = false;
                    for (const char* p2 = rs; *p2; ++p2) {
                        if ((*p2 >= '0' && *p2 <= '9') || *p2 == '-' || *p2 == '.') {
                            numeric = true; break;
                        }
                    }
                    if (!numeric) break;

                    const double v   = atof( rs );
                    const double lo  = atof( r.a.c_str() );
                    const double hi  = atof( r.b.c_str() );
                    if (v < lo || v > hi) {
                        violated = true;
                        char db[64];
                        Com_sprintf( db, sizeof(db), "outside [%s, %s]",
                                     r.a.c_str(), r.b.c_str() );
                        detail = db;
                    }
                    break;
                }
                case CVAR_NOTRANGE: {
                    const char* rs = result.c_str();
                    bool numeric = false;
                    for (const char* p2 = rs; *p2; ++p2) {
                        if ((*p2 >= '0' && *p2 <= '9') || *p2 == '-' || *p2 == '.') {
                            numeric = true; break;
                        }
                    }
                    if (!numeric) break;

                    const double v   = atof( rs );
                    const double lo  = atof( r.a.c_str() );
                    const double hi  = atof( r.b.c_str() );
                    if (v >= lo && v <= hi) {
                        violated = true;
                        char db[64];
                        Com_sprintf( db, sizeof(db), "inside [%s, %s]",
                                     r.a.c_str(), r.b.c_str() );
                        detail = db;
                    }
                    break;
                }
            }

            if (violated) {
                char rb[256];
                Com_sprintf( rb, sizeof(rb), "cvar rule failed: %s=%s %s",
                    name.c_str(), result.c_str(), detail.c_str() );
                recordViolation( clientNum, rb, r.sev );
            }
        }
        freePending( *p );
        return true;
    }

    // Admin-routed inspect reply (paks / modules list).  Never violation;
    // result is appended as a timestamped section to a per-target log file
    // under <mod>/inspections/ and the admin gets a one-line confirmation.
    //
    // Path: inspections/<safeName>_guid<guid>.log
    // The directory is created by the engine on first open via the VFS.
    if ((p->type == Q_PAKS || p->type == Q_MODULES_LIST)
        && p->requester >= 0 && p->requester < MAX_CLIENTS)
    {
        const User*  u           = resolveUser( clientNum );
        const string targetName  = asciiSafe( netname( clientNum ) );
        const char*  label       = (p->type == Q_PAKS) ? "PAKS" : "MODULES";

        // Sanitise name for a filesystem-safe filename.  Replace anything
        // outside [A-Za-z0-9_-] with underscore so colour codes, spaces,
        // and shell metacharacters can't escape the inspections dir.
        string safeName;
        safeName.reserve( targetName.length() );
        for (size_t i = 0; i < targetName.length(); ++i) {
            const char c = targetName[i];
            const bool ok = (c >= 'a' && c <= 'z')
                         || (c >= 'A' && c <= 'Z')
                         || (c >= '0' && c <= '9')
                         || c == '-';
            safeName += ok ? c : '_';
        }
        if (safeName.empty()) safeName = "unknown";

        const string guid = (u && !u->isNull()) ? u->guid : string("noguid");

        char path[256];
        Com_sprintf( path, sizeof(path),
            "inspections/%s_guid%s.log",
            safeName.c_str(), guid.c_str() );

        // Format the body neatly.
        string body;
        body.reserve( result.length() + 256 );
        int rendered = 0;
        if (p->type == Q_PAKS) {
            rendered = formatPaksList( result, body );
        } else {
            rendered = formatModulesList( result, body );
        }

        // Append: blank line + bracketed timestamp + labelled header + body.
        time_t now = time( NULL );
        char   tsbuf[64];
        strftime( tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", localtime( &now ));

        const char* noun = (p->type == Q_PAKS) ? "pak(s)" : "module(s)";

        char header[256];
        Com_sprintf( header, sizeof(header),
            "\n[%s] %s (target=%s) -- %d %s:\n",
            tsbuf, label, targetName.c_str(), rendered, noun );

        string section;
        section.reserve( body.length() + 256 );
        section += header;
        section += body;

        fileHandle_t f = 0;
        const int openRc = trap_FS_FOpenFile( path, &f, FS_APPEND );
        if (openRc >= 0 && f) {
            trap_FS_Write( section.c_str(), (int)section.length(), f );
            trap_FS_FCloseFile( f );

            trap_SendServerCommand( p->requester,
                va( "cpm \"^3AC inspect:^7 wrote %s of ^5%s^7 to ^5%s\n\"",
                    label, targetName.c_str(), path ) );
        } else {
            trap_SendServerCommand( p->requester,
                va( "cpm \"^1AC inspect:^7 failed to open ^5%s^7 for write\n\"",
                    path ) );
        }

        freePending( *p );
        return true;
    }

    // File-hash blocklist: walk every forbidden-hash rule that names this
    // path and fire a violation on each match.  Multiple rules with the
    // same path support blocking several known cheat variants at once.
    // Files not on the list pass through silently.
    if (p->type == Q_FHASH) {
        const string& path = p->args;

        // Skip empty / "err" / "big" replies — they're informational
        // (missing file, file too large) not a match.
        if (result.empty()
            || Q_stricmp( result.c_str(), "err" ) == 0
            || Q_stricmp( result.c_str(), "big" ) == 0)
        {
            G_LogPrintf( "AC: fhash client=%d path=%s result=%s (no action)\n",
                clientNum, path.c_str(), result.c_str() );
            freePending( *p );
            return true;
        }

        for (size_t k = 0; k < g_forbiddenHashes.size(); ++k) {
            const ForbiddenHash& fh = g_forbiddenHashes[k];
            if (Q_stricmp( fh.path.c_str(), path.c_str() ) != 0) continue;

            if (Q_stricmp( result.c_str(), fh.hexCrc32.c_str() ) == 0) {
                char rb[256];
                Com_sprintf( rb, sizeof(rb),
                    "forbidden file hash: %s = %s",
                    path.c_str(), result.c_str() );
                recordViolation( clientNum, rb, fh.sev );
            }
        }
        freePending( *p );
        return true;
    }

    // Module-list fingerprint: log it for allowlist building.  Phase 2 does
    // not auto-violation on modules; Phase 3 will add allowlist enforcement.
    if (p->type == Q_MODULES) {
        const User* u = resolveUser( clientNum );
        G_LogPrintf( "AC: modules client=%d (%s) guid=%s fingerprint=%s\n",
            clientNum, netname(clientNum),
            (u ? u->guid.c_str() : "<no-guid>"),
            result.c_str() );
        freePending( *p );
        return true;
    }

    // Memory pattern scan: only "hit" is a violation.  Anything else
    // ("miss", "unsupported" on Linux, "err:sig" if our signature tables
    // disagree) is logged but does NOT punish the client.
    if (p->type == Q_MEMPAT) {
        string r = result; str::toLower( r );
        if (r == "hit") {
            char buf[256];
            Com_sprintf( buf, sizeof(buf), "memory signature match: %s",
                p->args.c_str() );
            recordViolation( clientNum, buf, p->onMismatch );
        } else if (r != "miss") {
            // Log non-fatal responses for transparency.  No violation.
            G_LogPrintf( "AC: mempat client=%d sig=%s result=%s (no action)\n",
                clientNum, p->args.c_str(), result.c_str() );
        }
        freePending( *p );
        return true;
    }

    // Default path: compare to `expected` if non-empty.
    evaluateResponse( clientNum, *p, result );
    freePending( *p );
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Violation handler

void recordViolation( int clientNum, const char* reason, Severity sev ) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return;

    User* user = resolveUser( clientNum );
    const char* name = netname( clientNum );

    writeViolationLog( clientNum, reason, sev );

    // Accumulate soft score.
    if (sev == SEV_SOFT || sev == SEV_INFO) {
        g_clients[clientNum].violationsScore += (sev == SEV_SOFT ? 5 : 1);
    }

    const bool strict = (g_acStrict.integer != 0);

    if (sev == SEV_HARD && strict && user) {
        // Hard violation under strict mode -> immediate persistent ban.
        user->banned        = true;
        user->banTime       = time( NULL );
        user->banExpiry     = 0;  // permanent
        user->banReason     = string("AC: ") + reason;
        user->banAuthority  = "AC";
        user->banAuthorityx = "^1AC";

        // Persist the ban immediately so a quick reconnect before the next
        // periodic save can't bypass it.
        userDB.save();

        // Drop them.
        trap_DropClient( clientNum, va( "AC ban: %s", reason ),
                         /*duration*/ 0 );
        return;
    }

    if (sev == SEV_HARD && !strict) {
        // Strict mode off: still kick, but no persistent ban.  Admins can
        // investigate the log.
        trap_DropClient( clientNum, va( "AC kick: %s", reason ),
                         /*duration*/ 60 );
        return;
    }

    // Soft violations: ping admins (cpm to anyone with banPermanent privilege
    // is a decent proxy for "high-level admin").  Phase 2 will switch to a
    // dedicated AC-notify privilege.
    if (sev == SEV_SOFT) {
        const string safeName   = asciiSafe( name );
        const string safeReason = asciiSafe( reason );
        for (int i = 0; i < level.numConnectedClients; ++i) {
            const int slot = level.sortedClients[i];
            gentity_t* admin = &g_entities[slot];
            if (!admin->client) continue;
            if (!cmd::entityHasPermission( admin, priv::base::banPermanent )) continue;
            trap_SendServerCommand( slot,
                va( "cpm \"^1AC:^7 %s ^7- %s\n\"", safeName.c_str(), safeReason.c_str() ) );
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Admin-routed inspect query.

int scheduleInspectQuery( int targetSlot, QueryType type, int requesterSlot ) {
    if (targetSlot < 0 || targetSlot >= MAX_CLIENTS) return 0;
    if (type != Q_PAKS && type != Q_MODULES_LIST)    return 0;

    return scheduleQueryInternal( targetSlot, type,
        /*args=*/ "", /*expected=*/ "",
        /*onMismatch=*/ SEV_INFO,
        /*requester=*/ requesterSlot );
}

///////////////////////////////////////////////////////////////////////////////
// Registration

void registerForbiddenFileHash( const char* path, const char* hexCrc32,
                                Severity sev )
{
    if (!path || !hexCrc32) return;
    ForbiddenHash fh;
    fh.path     = path;
    fh.hexCrc32 = hexCrc32;
    str::toLower( fh.hexCrc32 );
    fh.sev      = sev;
    g_forbiddenHashes.push_back( fh );
}

void registerCvarRule( const char* cvarName, CvarRuleKind kind,
                       const char* a, const char* b, Severity sev )
{
    if (!cvarName || !*cvarName) return;

    CvarRule r;
    r.name = cvarName;
    r.kind = kind;
    r.a    = (a ? a : "");
    r.b    = (b ? b : "");
    r.sev  = sev;
    g_cvarRules.push_back( r );
}

void registerForbiddenCvarValue( const char* cvarName, const char* value,
                                 Severity sev )
{
    registerCvarRule( cvarName, CVAR_EQ, value, NULL, sev );
}

void registerMemoryPatternSig( const char* sigId, Severity sev ) {
    if (!sigId || !*sigId) return;
    MemPatSig s;
    s.id  = sigId;
    s.sev = sev;
    g_memPatSigs.push_back( s );
}

///////////////////////////////////////////////////////////////////////////////
// Policy loader.

namespace {

Severity parseSeverity( const string& tok ) {
    string s = tok;
    str::toLower( s );
    if (s == "hard") return SEV_HARD;
    if (s == "soft") return SEV_SOFT;
    return SEV_INFO;
}

// Split `line` on runs of whitespace.  Empty / pure-whitespace input -> empty
// out vector.  No quoting / escapes — config is hand-edited.
void tokenizeWhitespace( const string& line, vector<string>& out ) {
    out.clear();
    string cur;
    for (size_t i = 0; i < line.length(); ++i) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            if (!cur.empty()) { out.push_back( cur ); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back( cur );
}

// Split `line` on `delim`, trimming each result.  Simple version — no
// quoting / escapes (the config is meant to be hand-edited, not arbitrary).
void splitOn( const string& line, char delim, vector<string>& out ) {
    out.clear();
    string cur;
    for (size_t i = 0; i < line.length(); ++i) {
        if (line[i] == delim) { out.push_back( cur ); cur.clear(); }
        else                   cur += line[i];
    }
    out.push_back( cur );
    for (size_t i = 0; i < out.size(); ++i) {
        // trim ASCII whitespace
        size_t a = 0, b = out[i].length();
        while (a < b && (out[i][a] == ' ' || out[i][a] == '\t' || out[i][a] == '\r')) ++a;
        while (b > a && (out[i][b-1] == ' ' || out[i][b-1] == '\t' || out[i][b-1] == '\r')) --b;
        out[i] = out[i].substr( a, b - a );
    }
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// File-hash BLOCKLIST loader.
//
// File format (pipe-separated, `#` comments to end-of-line):
//
//   <path>|<crc32hex>|<severity>
//
// Semantics: each entry names a FORBIDDEN combination.  When a client's
// CRC32 of `<path>` equals `<crc32hex>`, the recorded severity fires.
// Multiple entries with the same `<path>` are supported — useful for
// blocking several known cheat variants of the same file.  Files not on
// the list are accepted silently.
//
// Severity: HARD | SOFT | INFO (default SOFT if omitted).
//
// Example:
//   pak3.pk3              | deadbeef | HARD     # known wallhack pak
//   cgame_mp_x86.dll      | cafebabe | HARD
//   cgame_mp_x86.dll      | f00dface | HARD     # different cheat, same dll
//
int loadFileHashPolicy( const char* configPath ) {
    if (!configPath || !*configPath) return 0;

    fileHandle_t f;
    const int len = trap_FS_FOpenFile( configPath, &f, FS_READ );
    if (len <= 0) {
        if (len == 0) trap_FS_FCloseFile( f );
        return 0;
    }

    // Cap config size at 64KB — anything larger is suspicious anyway.
    if (len > 65536) {
        trap_FS_FCloseFile( f );
        G_Printf( "AC: %s too large (%d bytes); skipping\n", configPath, len );
        return 0;
    }

    char* buf = new char[ len + 1 ];
    trap_FS_Read( buf, len, f );
    trap_FS_FCloseFile( f );
    buf[len] = '\0';

    int    loaded = 0;
    string line;

    for (int i = 0; i <= len; ++i) {
        const char c = (i < len) ? buf[i] : '\n';

        if (c == '\n' || c == '\r' || c == '\0') {
            // Trim leading whitespace.
            size_t a = 0;
            while (a < line.length() && (line[a] == ' ' || line[a] == '\t')) ++a;

            if (a < line.length() && line[a] != '#') {
                vector<string> fields;
                splitOn( line.substr( a ), '|', fields );

                if (fields.size() >= 2) {
                    const string&   path = fields[0];
                    const string&   crc  = fields[1];
                    Severity        sev  = (fields.size() >= 3)
                                            ? parseSeverity( fields[2] )
                                            : SEV_SOFT;
                    if (!path.empty() && crc.length() == 8) {
                        registerForbiddenFileHash( path.c_str(), crc.c_str(), sev );
                        ++loaded;
                    }
                }
            }

            line.clear();
        }
        else {
            line += c;
        }
    }

    delete[] buf;
    return loaded;
}

///////////////////////////////////////////////////////////////////////////////
// Cvar-rule policy loader.
//
// File format (whitespace-tokenised, `#` comments to end-of-line):
//
//   <cvarname>  <kind>  <args...>  <severity>
//
// Where <kind> is one of:
//   eq        <value>       -- violate when cvar == value
//   ne        <value>       -- violate when cvar != value
//   range     <min> <max>   -- violate when numeric value outside [min, max]
//   notrange  <min> <max>   -- violate when numeric value inside  [min, max]
//
// Severity: HARD | SOFT | INFO  (case-insensitive)
//
// Example:
//   r_showtris       eq        1            HARD
//   cg_fov           range     60   130     HARD
//   com_maxfps       range     30   250     SOFT
//
///////////////////////////////////////////////////////////////////////////////

int loadCvarRulePolicy( const char* configPath ) {
    if (!configPath || !*configPath) return 0;

    fileHandle_t f;
    const int len = trap_FS_FOpenFile( configPath, &f, FS_READ );
    if (len <= 0) {
        if (len == 0) trap_FS_FCloseFile( f );
        return 0;
    }

    if (len > 65536) {
        trap_FS_FCloseFile( f );
        G_Printf( "AC: %s too large (%d bytes); skipping\n", configPath, len );
        return 0;
    }

    char* buf = new char[ len + 1 ];
    trap_FS_Read( buf, len, f );
    trap_FS_FCloseFile( f );
    buf[len] = '\0';

    int    loaded = 0;
    int    lineNo = 0;
    string line;

    for (int i = 0; i <= len; ++i) {
        const char c = (i < len) ? buf[i] : '\n';

        if (c == '\n' || c == '\r' || c == '\0') {
            ++lineNo;

            // Strip `#`-to-EOL comments.
            {
                const size_t h = line.find( '#' );
                if (h != string::npos) line = line.substr( 0, h );
            }

            vector<string> tok;
            tokenizeWhitespace( line, tok );

            if (tok.size() >= 4) {
                const string& name = tok[0];
                string kindStr     = tok[1]; str::toLower( kindStr );
                const string& sevTok = tok[tok.size() - 1];
                Severity sev = parseSeverity( sevTok );

                CvarRuleKind kind = CVAR_EQ;
                bool ok = false;
                const char* aStr = NULL;
                const char* bStr = NULL;

                if ((kindStr == "eq" || kindStr == "ne") && tok.size() == 4) {
                    kind = (kindStr == "eq") ? CVAR_EQ : CVAR_NE;
                    aStr = tok[2].c_str();
                    ok = true;
                }
                else if ((kindStr == "range" || kindStr == "notrange")
                         && tok.size() == 5) {
                    kind = (kindStr == "range") ? CVAR_RANGE : CVAR_NOTRANGE;
                    aStr = tok[2].c_str();
                    bStr = tok[3].c_str();
                    ok = true;
                }

                if (ok) {
                    registerCvarRule( name.c_str(), kind, aStr, bStr, sev );
                    ++loaded;
                } else {
                    G_Printf( "AC: %s:%d malformed rule, ignored\n",
                              configPath, lineNo );
                }
            }
            else if (!tok.empty()) {
                G_Printf( "AC: %s:%d malformed rule, ignored\n",
                          configPath, lineNo );
            }

            line.clear();
        }
        else {
            line += c;
        }
    }

    delete[] buf;
    return loaded;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace AC
