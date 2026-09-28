#include "nhl_teams.h"

// Ids verified against the NHL's own team directory
// (api.nhle.com/stats/rest/en/team) on 2026-09-24 — see
// docs/features/nhl-api/README.md. Alphabetical by club name so the
// portal dropdowns scan naturally.
const NhlTeam NHL_TEAM_TABLE[] = {
  {0,   "",    "-- None --"},
  {24,  "ANA", "Anaheim Ducks (ANA)"},
  {6,   "BOS", "Boston Bruins (BOS)"},
  {7,   "BUF", "Buffalo Sabres (BUF)"},
  {20,  "CGY", "Calgary Flames (CGY)"},
  {12,  "CAR", "Carolina Hurricanes (CAR)"},
  {16,  "CHI", "Chicago Blackhawks (CHI)"},
  {21,  "COL", "Colorado Avalanche (COL)"},
  {29,  "CBJ", "Columbus Blue Jackets (CBJ)"},
  {25,  "DAL", "Dallas Stars (DAL)"},
  {17,  "DET", "Detroit Red Wings (DET)"},
  {22,  "EDM", "Edmonton Oilers (EDM)"},
  {13,  "FLA", "Florida Panthers (FLA)"},
  {26,  "LAK", "Los Angeles Kings (LAK)"},
  {30,  "MIN", "Minnesota Wild (MIN)"},
  {8,   "MTL", "Montreal Canadiens (MTL)"},
  {18,  "NSH", "Nashville Predators (NSH)"},
  {1,   "NJD", "New Jersey Devils (NJD)"},
  {2,   "NYI", "New York Islanders (NYI)"},
  {3,   "NYR", "New York Rangers (NYR)"},
  {9,   "OTT", "Ottawa Senators (OTT)"},
  {4,   "PHI", "Philadelphia Flyers (PHI)"},
  {5,   "PIT", "Pittsburgh Penguins (PIT)"},
  {28,  "SJS", "San Jose Sharks (SJS)"},
  {55,  "SEA", "Seattle Kraken (SEA)"},
  {19,  "STL", "St. Louis Blues (STL)"},
  {14,  "TBL", "Tampa Bay Lightning (TBL)"},
  {10,  "TOR", "Toronto Maple Leafs (TOR)"},
  {68,  "UTA", "Utah Mammoth (UTA)"},
  {23,  "VAN", "Vancouver Canucks (VAN)"},
  {54,  "VGK", "Vegas Golden Knights (VGK)"},
  {15,  "WSH", "Washington Capitals (WSH)"},
  {52,  "WPG", "Winnipeg Jets (WPG)"},
};
const size_t NHL_TEAM_TABLE_COUNT = sizeof(NHL_TEAM_TABLE) / sizeof(NHL_TEAM_TABLE[0]);

// Fresh installs start with no preferred teams (id 0 = slot unused); the
// setup portal is the first thing a new board shows, so the user picks
// their own priorities rather than inheriting ours.
const int NHL_DEFAULT_PREFERRED_TEAMS[3] = {0, 0, 0};

const char* nhlTeamAbbrev(int id, const char* fallbackStr) {
  for (size_t i = 0; i < NHL_TEAM_TABLE_COUNT; ++i) {
    if (NHL_TEAM_TABLE[i].id == id) {
      // Entry 0 is the "-- None --" placeholder; it carries no abbrev.
      if (NHL_TEAM_TABLE[i].abbrev[0] != '\0') return NHL_TEAM_TABLE[i].abbrev;
      break;
    }
  }
  return (fallbackStr && strlen(fallbackStr) > 0) ? fallbackStr : "NHL";
}

const NetworkTeamOption* nhlTeamOptions(size_t& count) {
  // Built once on first use (boot-time, not a render path).
  static NetworkTeamOption options[NHL_TEAM_TABLE_COUNT];
  static bool built = false;
  if (!built) {
    for (size_t i = 0; i < NHL_TEAM_TABLE_COUNT; ++i) {
      options[i].id = NHL_TEAM_TABLE[i].id;
      options[i].label = NHL_TEAM_TABLE[i].label;
    }
    built = true;
  }
  count = NHL_TEAM_TABLE_COUNT;
  return options;
}
