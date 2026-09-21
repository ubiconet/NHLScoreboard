#include "mlb_teams.h"

const MlbTeam MLB_TEAM_TABLE[] = {
  {0,   "",    "-- None --"},
  {108, "LAA", "Los Angeles Angels (LAA)"},
  {109, "ARI", "Arizona Diamondbacks (ARI)"},
  {110, "BAL", "Baltimore Orioles (BAL)"},
  {111, "BOS", "Boston Red Sox (BOS)"},
  {112, "CHC", "Chicago Cubs (CHC)"},
  {113, "CIN", "Cincinnati Reds (CIN)"},
  {114, "CLE", "Cleveland Guardians (CLE)"},
  {115, "COL", "Colorado Rockies (COL)"},
  {116, "DET", "Detroit Tigers (DET)"},
  {117, "HOU", "Houston Astros (HOU)"},
  {118, "KC",  "Kansas City Royals (KC)"},
  {119, "LAD", "Los Angeles Dodgers (LAD)"},
  {120, "WSH", "Washington Nationals (WSH)"},
  {121, "NYM", "New York Mets (NYM)"},
  {133, "OAK", "Oakland Athletics (ATH)"},
  {134, "PIT", "Pittsburgh Pirates (PIT)"},
  {135, "SD",  "San Diego Padres (SD)"},
  {136, "SEA", "Seattle Mariners (SEA)"},
  {137, "SF",  "San Francisco Giants (SF)"},
  {138, "STL", "St. Louis Cardinals (STL)"},
  {139, "TB",  "Tampa Bay Rays (TB)"},
  {140, "TEX", "Texas Rangers (TEX)"},
  {141, "TOR", "Toronto Blue Jays (TOR)"},
  {142, "MIN", "Minnesota Twins (MIN)"},
  {143, "PHI", "Philadelphia Phillies (PHI)"},
  {144, "ATL", "Atlanta Braves (ATL)"},
  {145, "CWS", "Chicago White Sox (CWS)"},
  {146, "MIA", "Miami Marlins (MIA)"},
  {147, "NYY", "New York Yankees (NYY)"},
  {158, "MIL", "Milwaukee Brewers (MIL)"},
};
const size_t MLB_TEAM_TABLE_COUNT = sizeof(MLB_TEAM_TABLE) / sizeof(MLB_TEAM_TABLE[0]);

const int MLB_DEFAULT_PREFERRED_TEAMS[3] = {143, 144, 111};  // PHI, ATL, BOS

const char* mlbTeamAbbrev(int id, const char* fallbackStr) {
  for (size_t i = 0; i < MLB_TEAM_TABLE_COUNT; ++i) {
    if (MLB_TEAM_TABLE[i].id == id) {
      // Entry 0 is the "-- None --" placeholder; it carries no abbrev.
      if (MLB_TEAM_TABLE[i].abbrev[0] != '\0') return MLB_TEAM_TABLE[i].abbrev;
      break;
    }
  }
  return (fallbackStr && strlen(fallbackStr) > 0) ? fallbackStr : "MLB";
}

const NetworkTeamOption* mlbTeamOptions(size_t& count) {
  // Built once on first use (boot-time, not a render path).
  static NetworkTeamOption options[MLB_TEAM_TABLE_COUNT];
  static bool built = false;
  if (!built) {
    for (size_t i = 0; i < MLB_TEAM_TABLE_COUNT; ++i) {
      options[i].id = MLB_TEAM_TABLE[i].id;
      options[i].label = MLB_TEAM_TABLE[i].label;
    }
    built = true;
  }
  count = MLB_TEAM_TABLE_COUNT;
  return options;
}
