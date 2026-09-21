#pragma once

#include <Arduino.h>

#include "common/comms/network_service.h"

// Single source of truth for MLB team identity. The id->abbrev mapping used
// by the renderer/logo cache and the id->label list used by the setup
// portal's dropdowns historically lived in two files; they are one table now.
// The label is the exact portal display string (which sometimes differs from
// the canonical abbrev, e.g. Oakland: label ATH, canonical OAK).

struct MlbTeam {
  int id;
  const char* abbrev;   // canonical 3-letter code (logos, live screen)
  const char* label;    // setup-portal dropdown text
};

extern const MlbTeam MLB_TEAM_TABLE[];
extern const size_t MLB_TEAM_TABLE_COUNT;

// Preferred-team ids preloaded when NVS has none saved yet (PHI/ATL/BOS).
extern const int MLB_DEFAULT_PREFERRED_TEAMS[3];

// Canonical abbreviation for a team id, or `fallback` (defaults to "MLB")
// when the id is unknown and no fallback string is supplied.
const char* mlbTeamAbbrev(int teamId, const char* fallbackStr = nullptr);

// Portal-ready view of the table (id + dropdown label) for injection into
// the generic network service.
const NetworkTeamOption* mlbTeamOptions(size_t& count);
