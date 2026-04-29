// Jaymod-AC: minimal GeoIP country lookup
//
// Reads a legacy MaxMind GeoIP.dat (Country edition) from the ET filesystem
// at game init, keeps the binary tree in memory, and resolves IPv4 addresses
// to country names on demand.
//
// Only the Country edition (record_length = 3, COUNTRY_BEGIN = 16776960) is
// supported. Other editions (City, ISP, ASN, IPv6, MMDB v2) will be rejected.

#ifndef G_GEOIP_H
#define G_GEOIP_H

// Initialize from the path in g_geoipFile. Safe to call repeatedly; reloads
// on each call. Logs success/failure via G_Printf. No-op if g_geoipFile is
// empty.
void G_GeoIP_Init();

// Free in-memory database. Safe to call when not initialized.
void G_GeoIP_Shutdown();

// Look up a country name for a dotted-quad IPv4 string ("1.2.3.4" or
// "1.2.3.4:port"). Returns a static string; never NULL. Returns "Unknown"
// if the database isn't loaded or the address can't be resolved.
const char* G_GeoIP_CountryByIP(const char* ip);

#endif // G_GEOIP_H
