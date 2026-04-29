#include <bgame/impl.h>

namespace cmd {

///////////////////////////////////////////////////////////////////////////////

SetProfile::SetProfile()
    : AbstractBuiltin( "setprofile", true /* grantAlways: self-service only */ )
{
    __usage << xvalue( "!" + _name ) << ' '
            << xvalue( "title|sig|clan|clear" ) << ' '
            << _ovalue( "TEXT" );
    __descr << "Set your own profile customization (title, signature, or clan tag). "
            << "`clear FIELD` to remove. `clear all` to wipe all three.";
}

///////////////////////////////////////////////////////////////////////////////

SetProfile::~SetProfile()
{
}

///////////////////////////////////////////////////////////////////////////////

namespace {

// Concatenate args[from..end-1] with single-space separators.
string joinArgs( const vector<string>& args, size_t from ) {
    string s;
    for (size_t i = from; i < args.size(); ++i) {
        if (i > from) s += ' ';
        s += args[i];
    }
    return s;
}

// Visible length: total length minus color-code escapes. Caps the user
// from sneaking 200 chars of color-padded content into a "32-char" field.
size_t visibleLength( const string& s ) {
    size_t n = 0;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == Q_COLOR_ESCAPE && (i + 1) < s.length()) {
            ++i; continue;
        }
        ++n;
    }
    return n;
}

void clearField( User& u, const string& field, bool& cleared ) {
    if      (field == "title") { u.customTitle.clear();     cleared = true; }
    else if (field == "sig")   { u.customSignature.clear(); cleared = true; }
    else if (field == "clan")  { u.customClanTag.clear();   cleared = true; }
    else if (field == "all") {
        u.customTitle.clear();
        u.customSignature.clear();
        u.customClanTag.clear();
        cleared = true;
    }
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

AbstractCommand::PostAction
SetProfile::doExecute( Context& txt )
{
    if (txt._args.size() < 2)
        return PA_USAGE;

    if (!txt._client) {
        txt._ebuf << "Console cannot set a profile (no User to write to).";
        return PA_ERROR;
    }

    User* user = connectedUsers[txt._client->slot];
    if (!user || user->isNull() || user->fakeguid) {
        txt._ebuf << "You don't have a profile to edit (no GUID).";
        return PA_ERROR;
    }

    string sub = txt._args[1];
    str::toLower( sub );

    // ----- clear -----
    if (sub == "clear") {
        if (txt._args.size() != 3)
            return PA_USAGE;

        string what = txt._args[2];
        str::toLower( what );

        bool cleared = false;
        clearField( *user, what, cleared );

        if (!cleared) {
            txt._ebuf << "Unknown field " << xvalue( what )
                      << " — try " << xvalue( "title" )
                      << ", "      << xvalue( "sig" )
                      << ", "      << xvalue( "clan" )
                      << " or "    << xvalue( "all" ) << ".";
            return PA_ERROR;
        }

        Buffer buf;
        buf << "Cleared " << xvalue( what ) << ".";
        print( txt._client, buf );
        return PA_NONE;
    }

    // ----- set -----
    if (txt._args.size() < 3)
        return PA_USAGE;

    string text = joinArgs( txt._args, 2 );

    // Censor pass — but only if g_censor is active.  Sets clean text
    // back without penalising; the player just gets the cleaned version.
    if (g_censor.integer && !cmd::entityHasPermission(
            &g_entities[txt._client->slot], priv::base::censorImmunity )) {
        censorDB.filter( text );
    }

    if (sub == "title") {
        if (visibleLength( text ) > User::CUSTOM_TITLE_MAX) {
            txt._ebuf << "Title exceeds " << xvalue( (int)User::CUSTOM_TITLE_MAX )
                      << " visible characters.";
            return PA_ERROR;
        }
        user->customTitle = text;
        Buffer buf;
        buf << "Title set to " << xvalue( text ) << ".";
        print( txt._client, buf );
        return PA_NONE;
    }

    if (sub == "sig") {
        if (visibleLength( text ) > User::CUSTOM_SIG_MAX) {
            txt._ebuf << "Signature exceeds " << xvalue( (int)User::CUSTOM_SIG_MAX )
                      << " visible characters.";
            return PA_ERROR;
        }
        user->customSignature = text;
        Buffer buf;
        buf << "Signature set.";
        print( txt._client, buf );
        return PA_NONE;
    }

    if (sub == "clan") {
        if (visibleLength( text ) > User::CUSTOM_CLAN_MAX) {
            txt._ebuf << "Clan tag exceeds " << xvalue( (int)User::CUSTOM_CLAN_MAX )
                      << " visible characters.";
            return PA_ERROR;
        }
        user->customClanTag = text;
        Buffer buf;
        buf << "Clan tag set to " << xvalue( text ) << ".";
        print( txt._client, buf );
        return PA_NONE;
    }

    return PA_USAGE;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace cmd
