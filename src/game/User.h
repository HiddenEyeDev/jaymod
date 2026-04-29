#ifndef GAME_USER_H
#define GAME_USER_H

///////////////////////////////////////////////////////////////////////////////

class UserDB;

///////////////////////////////////////////////////////////////////////////////

/*
 * User class represents a persistent data record for every unique
 * GUID-based player.
 *
 * For all practical purposes we assume GUID key is guaranteed to be unique
 * and the success of this implementation hinges on that assumption.
 *
 */
class User {
    friend class UserDB;

private:
    string _guid;
    
public:
    User ( );
    User ( bool );
    User ( const User& );

    ~User ();

    bool  operator== ( const User& ) const;
    bool  operator!= ( const User& ) const;
    User& operator=  ( const User& );

    void canonicalizePrivileges();

    bool hasPrivilege ( const Privilege& ) const;
    void reset        ( );
    void xpReset      ( );

    /**************************************************************************
     * Returns exactly: this == &BAD .
     */
    bool isNull() const;

    /**************************************************************************
     * Main attributes.
     */
    const string&  guid;           // globally unique ID assigned by punkbuster
    bool           fakeguid;       // track locally assigned guids
    time_t         timestamp;      // time of last connect/disconnect
    string         ip;             // client IPv4 address updated on connect
    string         mac;            // client MAC address updated on connect
    string         name;           // client name updated on connect and change
    string         namex;          // name w/ extended-color-codes
    string         greetingText;   // broadcast greeting on connect
    string         greetingAudio;  // audio to play on connect
    int            authLevel;      // authorization level

    PrivilegeSet* privGranted;  // per-user granted
    PrivilegeSet* privDenied;   // per-user denied

    /**************************************************************************
     * Jaymod-AC: lifetime player profile / stats.
     *
     * Bumped from g_combat.cpp (kills/deaths/headshots/streak) and g_session.cpp
     * + g_client.cpp (playtime, firstSeen).  Persisted via encode/decode.
     */
    int    statKills;            // lifetime kills (player vs player)
    int    statDeaths;           // lifetime deaths
    int    statHeadshots;        // lifetime headshot kills
    int    statLongestStreak;    // best kill streak across all sessions
    int    statPlaytimeSecs;     // total connected time in seconds
    int    statShotsFired;       // lifetime shots fired (sum across weapons)
    int    statShotsHit;         // lifetime shots hit (sum across weapons)
    time_t firstSeen;            // unix-time of first connect (0 = unknown)

    // Returns a 0..1000 skill rating computed from the lifetime stats above.
    // Phase 2 weighting: K/D 40%, accuracy 35%, kills 15%, streak 10%.
    int    computeSkillRating() const;

    // Returns 0..1 accuracy ratio (hits / fired). 0 if no shots fired.
    float  computeAccuracy() const;

    /**************************************************************************
     * Jaymod-AC: achievements.  Indexed by Ach::Id.  0 = locked.
     * Non-repeatable IDs clamp at 1; repeatable IDs increment freely.
     *
     * Sized larger than Ach::NUM to leave headroom for new achievements
     * without re-touching this header; g_achievements.cpp asserts NUM fits.
     */
    static const int ACH_MAX = 64;
    int    achCount[ ACH_MAX ];

    // Number of distinct achievements with achCount[i] > 0.
    int    achievementsUnlocked() const;

    /**************************************************************************
     * Jaymod-AC: rank / XP progression and community reputation.
     *
     *   - statXp      is computed on demand from kills, headshots, and
     *                 achievements; we don't store it.
     *   - statRank    is the cached rank (1..MAX_RANK) so we can detect a
     *                 promotion since the last save and broadcast it.
     *   - statRep+/-  are the lifetime up/down votes from `!rep`.
     */
    int  statRank;          // cached rank tier; 0 or 1 means "fresh"
    int  statRepPositive;   // lifetime up-votes received
    int  statRepNegative;   // lifetime down-votes received

    /**************************************************************************
     * Jaymod-AC: customization (Phase 5).  Set via `!setprofile`.  Empty by
     * default.  Server admins can purge offensive entries by editing user.db.
     */
    string customTitle;     // shown in !profile / !card. Max ~32 visible chars.
    string customSignature; // shown in !profile.       Max ~80 visible chars.
    string customClanTag;   // short clan tag.          Max  ~8 visible chars.

    static const size_t CUSTOM_TITLE_MAX = 32;
    static const size_t CUSTOM_SIG_MAX   = 80;
    static const size_t CUSTOM_CLAN_MAX  = 8;

    int  computeXp()   const;     // derived XP total from current stats
    int  computeRank() const;     // 1..MAX_RANK from computeXp()

    static int xpForRank ( int rank );  // rank -> XP threshold lower bound
    static int rankForXp ( int xp );    // XP   -> rank
    static const int MAX_RANK = 20;

    /**************************************************************************
     * XP-save attributes.
     */
    float  xpSkills[ SK_NUM_SKILLS ];  // XP for each skill

    /**************************************************************************
     * Mute attributes.
     *
     */
    bool   muted;           // true when muted, when false mute* fields are ignored
    time_t muteTime;        // time of mute
    time_t muteExpiry;      // when mute expires
    string muteReason;      // reason for mute
    string muteAuthority;   // name of user who muted this user
    string muteAuthorityx;  // muteAuthority w/ extended-color-codes

    /**************************************************************************
     * Ban attributes.
     */
    bool   banned;         // true when banned, when false ban* fields are ignored
    time_t banTime;        // time of ban
    time_t banExpiry;      // time when ban expires, 0 == permanent
    string banReason;      // reason for ban
    string banAuthority;   // name of user who banned this user
    string banAuthorityx;  // banAuthority w/ extended-color-codes

    /**************************************************************************
     * Note attributes.
     */
    vector<string> notes;

private:
    void  decode ( map<string,string>& );
    void  encode ( ostream&, int );

public:
    static User BAD;
    static User DEFAULT;
    static User CONSOLE;

    static const vector<string>::size_type notesMax;

private:
    static void scramble( char*, int );
};

///////////////////////////////////////////////////////////////////////////////

#endif // GAME_USER_H
