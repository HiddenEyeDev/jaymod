// Jaymod-AC: minimal GeoIP country lookup. See g_geoip.h for usage.
//
// Implements the legacy MaxMind GeoIP.dat Country-edition tree walk:
//   - 32-bit IPv4 in network order, walked MSB-first
//   - each node = 6 bytes (two 3-byte little-endian-ish pointers)
//   - pointer >= COUNTRY_BEGIN (16776960) means "leaf"; country index =
//     pointer - COUNTRY_BEGIN
//
// We embed the full country code/name tables so a single .dat is enough.

#include <bgame/impl.h>
#include "g_geoip.h"

namespace {

const unsigned int COUNTRY_BEGIN     = 16776960u;
const int          RECORD_LENGTH     = 3;     // bytes per pointer
const int          NODE_SIZE         = RECORD_LENGTH * 2;
const int          MAX_DEPTH         = 32;    // IPv4 bits

// Country code/name tables — index aligned with the legacy MaxMind ordering.
// First entry ("--", "N/A") is the sentinel for "unknown".
const char* const kCountryName[] = {
    "N/A",                              // 0
    "Asia/Pacific Region",              // 1 AP
    "Europe",                           // 2 EU
    "Andorra",                          // 3 AD
    "United Arab Emirates",             // 4 AE
    "Afghanistan",                      // 5 AF
    "Antigua and Barbuda",              // 6 AG
    "Anguilla",                         // 7 AI
    "Albania",                          // 8 AL
    "Armenia",                          // 9 AM
    "Curacao",                          // 10 CW
    "Angola",                           // 11 AO
    "Antarctica",                       // 12 AQ
    "Argentina",                        // 13 AR
    "American Samoa",                   // 14 AS
    "Austria",                          // 15 AT
    "Australia",                        // 16 AU
    "Aruba",                            // 17 AW
    "Azerbaijan",                       // 18 AZ
    "Bosnia and Herzegovina",           // 19 BA
    "Barbados",                         // 20 BB
    "Bangladesh",                       // 21 BD
    "Belgium",                          // 22 BE
    "Burkina Faso",                     // 23 BF
    "Bulgaria",                         // 24 BG
    "Bahrain",                          // 25 BH
    "Burundi",                          // 26 BI
    "Benin",                            // 27 BJ
    "Bermuda",                          // 28 BM
    "Brunei Darussalam",                // 29 BN
    "Bolivia",                          // 30 BO
    "Brazil",                           // 31 BR
    "Bahamas",                          // 32 BS
    "Bhutan",                           // 33 BT
    "Bouvet Island",                    // 34 BV
    "Botswana",                         // 35 BW
    "Belarus",                          // 36 BY
    "Belize",                           // 37 BZ
    "Canada",                           // 38 CA
    "Cocos (Keeling) Islands",          // 39 CC
    "Congo, The Democratic Republic of the", // 40 CD
    "Central African Republic",         // 41 CF
    "Congo",                            // 42 CG
    "Switzerland",                      // 43 CH
    "Cote D'Ivoire",                    // 44 CI
    "Cook Islands",                     // 45 CK
    "Chile",                            // 46 CL
    "Cameroon",                         // 47 CM
    "China",                            // 48 CN
    "Colombia",                         // 49 CO
    "Costa Rica",                       // 50 CR
    "Cuba",                             // 51 CU
    "Cape Verde",                       // 52 CV
    "Christmas Island",                 // 53 CX
    "Cyprus",                           // 54 CY
    "Czech Republic",                   // 55 CZ
    "Germany",                          // 56 DE
    "Djibouti",                         // 57 DJ
    "Denmark",                          // 58 DK
    "Dominica",                         // 59 DM
    "Dominican Republic",               // 60 DO
    "Algeria",                          // 61 DZ
    "Ecuador",                          // 62 EC
    "Estonia",                          // 63 EE
    "Egypt",                            // 64 EG
    "Western Sahara",                   // 65 EH
    "Eritrea",                          // 66 ER
    "Spain",                            // 67 ES
    "Ethiopia",                         // 68 ET
    "Finland",                          // 69 FI
    "Fiji",                             // 70 FJ
    "Falkland Islands (Malvinas)",      // 71 FK
    "Micronesia, Federated States of",  // 72 FM
    "Faroe Islands",                    // 73 FO
    "France",                           // 74 FR
    "Sint Maarten (Dutch part)",        // 75 SX
    "Gabon",                            // 76 GA
    "United Kingdom",                   // 77 GB
    "Grenada",                          // 78 GD
    "Georgia",                          // 79 GE
    "French Guiana",                    // 80 GF
    "Ghana",                            // 81 GH
    "Gibraltar",                        // 82 GI
    "Greenland",                        // 83 GL
    "Gambia",                           // 84 GM
    "Guinea",                           // 85 GN
    "Guadeloupe",                       // 86 GP
    "Equatorial Guinea",                // 87 GQ
    "Greece",                           // 88 GR
    "South Georgia and the South Sandwich Islands", // 89 GS
    "Guatemala",                        // 90 GT
    "Guam",                             // 91 GU
    "Guinea-Bissau",                    // 92 GW
    "Guyana",                           // 93 GY
    "Hong Kong",                        // 94 HK
    "Heard Island and McDonald Islands",// 95 HM
    "Honduras",                         // 96 HN
    "Croatia",                          // 97 HR
    "Haiti",                            // 98 HT
    "Hungary",                          // 99 HU
    "Indonesia",                        // 100 ID
    "Ireland",                          // 101 IE
    "Israel",                           // 102 IL
    "India",                            // 103 IN
    "British Indian Ocean Territory",   // 104 IO
    "Iraq",                             // 105 IQ
    "Iran, Islamic Republic of",        // 106 IR
    "Iceland",                          // 107 IS
    "Italy",                            // 108 IT
    "Jamaica",                          // 109 JM
    "Jordan",                           // 110 JO
    "Japan",                            // 111 JP
    "Kenya",                            // 112 KE
    "Kyrgyzstan",                       // 113 KG
    "Cambodia",                         // 114 KH
    "Kiribati",                         // 115 KI
    "Comoros",                          // 116 KM
    "Saint Kitts and Nevis",            // 117 KN
    "Korea, Democratic People's Republic of", // 118 KP
    "Korea, Republic of",               // 119 KR
    "Kuwait",                           // 120 KW
    "Cayman Islands",                   // 121 KY
    "Kazakhstan",                       // 122 KZ
    "Lao People's Democratic Republic", // 123 LA
    "Lebanon",                          // 124 LB
    "Saint Lucia",                      // 125 LC
    "Liechtenstein",                    // 126 LI
    "Sri Lanka",                        // 127 LK
    "Liberia",                          // 128 LR
    "Lesotho",                          // 129 LS
    "Lithuania",                        // 130 LT
    "Luxembourg",                       // 131 LU
    "Latvia",                           // 132 LV
    "Libya",                            // 133 LY
    "Morocco",                          // 134 MA
    "Monaco",                           // 135 MC
    "Moldova, Republic of",             // 136 MD
    "Madagascar",                       // 137 MG
    "Marshall Islands",                 // 138 MH
    "North Macedonia",                  // 139 MK
    "Mali",                             // 140 ML
    "Myanmar",                          // 141 MM
    "Mongolia",                         // 142 MN
    "Macao",                            // 143 MO
    "Northern Mariana Islands",         // 144 MP
    "Martinique",                       // 145 MQ
    "Mauritania",                       // 146 MR
    "Montserrat",                       // 147 MS
    "Malta",                            // 148 MT
    "Mauritius",                        // 149 MU
    "Maldives",                         // 150 MV
    "Malawi",                           // 151 MW
    "Mexico",                           // 152 MX
    "Malaysia",                         // 153 MY
    "Mozambique",                       // 154 MZ
    "Namibia",                          // 155 NA
    "New Caledonia",                    // 156 NC
    "Niger",                            // 157 NE
    "Norfolk Island",                   // 158 NF
    "Nigeria",                          // 159 NG
    "Nicaragua",                        // 160 NI
    "Netherlands",                      // 161 NL
    "Norway",                           // 162 NO
    "Nepal",                            // 163 NP
    "Nauru",                            // 164 NR
    "Niue",                             // 165 NU
    "New Zealand",                      // 166 NZ
    "Oman",                             // 167 OM
    "Panama",                           // 168 PA
    "Peru",                             // 169 PE
    "French Polynesia",                 // 170 PF
    "Papua New Guinea",                 // 171 PG
    "Philippines",                      // 172 PH
    "Pakistan",                         // 173 PK
    "Poland",                           // 174 PL
    "Saint Pierre and Miquelon",        // 175 PM
    "Pitcairn Islands",                 // 176 PN
    "Puerto Rico",                      // 177 PR
    "Palestinian Territory",            // 178 PS
    "Portugal",                         // 179 PT
    "Palau",                            // 180 PW
    "Paraguay",                         // 181 PY
    "Qatar",                            // 182 QA
    "Reunion",                          // 183 RE
    "Romania",                          // 184 RO
    "Russian Federation",               // 185 RU
    "Rwanda",                           // 186 RW
    "Saudi Arabia",                     // 187 SA
    "Solomon Islands",                  // 188 SB
    "Seychelles",                       // 189 SC
    "Sudan",                            // 190 SD
    "Sweden",                           // 191 SE
    "Singapore",                        // 192 SG
    "Saint Helena",                     // 193 SH
    "Slovenia",                         // 194 SI
    "Svalbard and Jan Mayen",           // 195 SJ
    "Slovakia",                         // 196 SK
    "Sierra Leone",                     // 197 SL
    "San Marino",                       // 198 SM
    "Senegal",                          // 199 SN
    "Somalia",                          // 200 SO
    "Suriname",                         // 201 SR
    "Sao Tome and Principe",            // 202 ST
    "El Salvador",                      // 203 SV
    "Syrian Arab Republic",             // 204 SY
    "Eswatini",                         // 205 SZ
    "Turks and Caicos Islands",         // 206 TC
    "Chad",                             // 207 TD
    "French Southern Territories",      // 208 TF
    "Togo",                             // 209 TG
    "Thailand",                         // 210 TH
    "Tajikistan",                       // 211 TJ
    "Tokelau",                          // 212 TK
    "Turkmenistan",                     // 213 TM
    "Tunisia",                          // 214 TN
    "Tonga",                            // 215 TO
    "Timor-Leste",                      // 216 TL
    "Turkey",                           // 217 TR
    "Trinidad and Tobago",              // 218 TT
    "Tuvalu",                           // 219 TV
    "Taiwan",                           // 220 TW
    "Tanzania, United Republic of",     // 221 TZ
    "Ukraine",                          // 222 UA
    "Uganda",                           // 223 UG
    "United States Minor Outlying Islands", // 224 UM
    "United States",                    // 225 US
    "Uruguay",                          // 226 UY
    "Uzbekistan",                       // 227 UZ
    "Holy See (Vatican City State)",    // 228 VA
    "Saint Vincent and the Grenadines", // 229 VC
    "Venezuela",                        // 230 VE
    "Virgin Islands, British",          // 231 VG
    "Virgin Islands, U.S.",             // 232 VI
    "Vietnam",                          // 233 VN
    "Vanuatu",                          // 234 VU
    "Wallis and Futuna",                // 235 WF
    "Samoa",                            // 236 WS
    "Yemen",                            // 237 YE
    "Mayotte",                          // 238 YT
    "Serbia",                           // 239 RS
    "South Africa",                     // 240 ZA
    "Zambia",                           // 241 ZM
    "Montenegro",                       // 242 ME
    "Zimbabwe",                         // 243 ZW
    "Anonymous Proxy",                  // 244 A1
    "Satellite Provider",               // 245 A2
    "Other",                            // 246 O1
    "Aland Islands",                    // 247 AX
    "Guernsey",                         // 248 GG
    "Isle of Man",                      // 249 IM
    "Jersey",                           // 250 JE
    "Saint Barthelemy",                 // 251 BL
    "Saint Martin",                     // 252 MF
    "Bonaire, Saint Eustatius and Saba",// 253 BQ
    "South Sudan",                      // 254 SS
    "Other"                             // 255 O1
};
const int kCountryNameCount = sizeof(kCountryName) / sizeof(kCountryName[0]);

// In-memory copy of GeoIP.dat. NULL when not loaded.
unsigned char* gDB     = NULL;
int            gDBSize = 0;

// Parse "1.2.3.4" or "1.2.3.4:port" into a 32-bit network-order integer
// (high octet = first octet). Returns false on failure.
bool parseIPv4(const char* s, unsigned int& out) {
    if (!s || !*s) return false;
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    if ((a | b | c | d) & ~0xFF) return false;
    out = ((unsigned int)a << 24) | ((unsigned int)b << 16) |
          ((unsigned int)c << 8)  |  (unsigned int)d;
    return true;
}

} // namespace

void G_GeoIP_Init() {
    G_GeoIP_Shutdown();

    const char* path = g_geoipFile.string;
    if (!path || !*path) {
        G_Printf("GeoIP: disabled (g_geoipFile is empty)\n");
        return;
    }

    fileHandle_t f;
    int len = trap_FS_FOpenFile(path, &f, FS_READ);
    if (len <= 0) {
        G_Printf("GeoIP: unable to open %s (country lookup disabled)\n", path);
        if (len == 0) trap_FS_FCloseFile(f);
        return;
    }

    gDB = new unsigned char[len];
    gDBSize = len;
    trap_FS_Read(gDB, len, f);
    trap_FS_FCloseFile(f);

    G_Printf("GeoIP: loaded %s (%d bytes)\n", path, len);
}

void G_GeoIP_Shutdown() {
    if (gDB) {
        delete[] gDB;
        gDB = NULL;
    }
    gDBSize = 0;
}

const char* G_GeoIP_CountryByIP(const char* ip) {
    if (!gDB || gDBSize < NODE_SIZE) return "Unknown";

    unsigned int ipnum;
    if (!parseIPv4(ip, ipnum)) return "Unknown";

    unsigned int offset = 0;
    for (int depth = MAX_DEPTH - 1; depth >= 0; --depth) {
        // Bounds-check the node we're about to read.
        unsigned int byteOffset = offset * NODE_SIZE;
        if (byteOffset + NODE_SIZE > (unsigned int)gDBSize) {
            return "Unknown";
        }

        const unsigned char* node = gDB + byteOffset;
        unsigned int next;
        if (ipnum & (1u << depth)) {
            // right pointer: bytes 3..5
            next = (unsigned int)node[3]
                 | ((unsigned int)node[4] << 8)
                 | ((unsigned int)node[5] << 16);
        } else {
            // left pointer: bytes 0..2
            next = (unsigned int)node[0]
                 | ((unsigned int)node[1] << 8)
                 | ((unsigned int)node[2] << 16);
        }

        if (next >= COUNTRY_BEGIN) {
            unsigned int idx = next - COUNTRY_BEGIN;
            if (idx == 0 || idx >= (unsigned int)kCountryNameCount) {
                return "Unknown";
            }
            return kCountryName[idx];
        }

        offset = next;
    }

    return "Unknown";
}
