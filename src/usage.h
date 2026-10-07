#pragma once
#include <string>

struct UsageInfo {
  bool ok = false;
  std::string error;
  bool hasSession = false;
  double sessionPct = 0;
  std::string sessionResetsAt;
  bool hasWeekly = false;
  double weeklyPct = 0;
  std::string weeklyResetsAt;
};

// Blocking: reads the Claude CLI's stored OAuth token and calls the (undocumented) subscription
// usage endpoint. Call from a background thread only.
UsageInfo fetchClaudeUsage();
