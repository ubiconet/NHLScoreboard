#pragma once

#include <Arduino.h>

#include "common/comms/network_service.h"

// Single source of truth for NHL team identity. The id->abbrev mapping used
// by the renderer/logo cache and the id->label list used by the setup
// portal's dropdowns historically lived in two files; they are one table now.
// The label is the exact portal display string; the abbrev is the NHL's
// canonical triCode (matches assets.nhle.com logo URLs and API payloads).

struct NhlTeam {
  int id;
  const char* abbrev;   // canonical 3-letter code (logos, live screen)
  const char* label;    // setup-portal dropdown text
};

extern const NhlTeam NHL_TEAM_TABLE[];
extern const size_t NHL_TEAM_TABLE_COUNT;

// Preferred-team ids preloaded when NVS has none saved yet — all unused
// (0) on a fresh install; the user picks priorities in the setup portal.
extern const int NHL_DEFAULT_PREFERRED_TEAMS[3];

// Canonical abbreviation for a team id, or `fallback` (defaults to "NHL")
// when the id is unknown and no fallback string is supplied.
const char* nhlTeamAbbrev(int teamId, const char* fallbackStr = nullptr);

// Portal-ready view of the table (id + dropdown label) for injection into
// the generic network service.
const NetworkTeamOption* nhlTeamOptions(size_t& count);
