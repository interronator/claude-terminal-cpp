#include "usage.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "platform.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string credentialsPath() {
  if (const char* cfg = std::getenv("CLAUDE_CONFIG_DIR")) return (fs::path(cfg) / ".credentials.json").string();
#ifdef _WIN32
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  fs::path dir = home ? fs::path(home) : fs::path(".");
  return (dir / ".claude" / ".credentials.json").string();
}

const char* kNotLoggedIn = "Not logged in to Claude - run `claude` once to sign in, then try again.";

}  // namespace

UsageInfo fetchClaudeUsage() {
  UsageInfo out;
  std::ifstream in(credentialsPath());
  if (!in) {
    out.error = kNotLoggedIn;
    return out;
  }
  json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
  std::string token;
  if (!j.is_discarded() && j.contains("claudeAiOauth") && j["claudeAiOauth"].is_object())
    token = j["claudeAiOauth"].value("accessToken", "");
  if (token.empty()) {
    out.error = kNotLoggedIn;
    return out;
  }

  auto res = platform::httpGetJson("https://api.anthropic.com/api/oauth/usage",
                                   {
                                       {"Authorization", "Bearer " + token},
                                       {"anthropic-beta", "oauth-2025-04-20"},
                                       {"Content-Type", "application/json"},
                                   });
  if (!res.ok) {
    out.error = res.error.empty() ? "Could not reach the usage endpoint." : res.error;
    return out;
  }
  if (res.statusCode == 401) {
    out.error = "Could not fetch usage - try running a Claude session first to refresh your login.";
    return out;
  }
  if (res.statusCode != 200) {
    out.error = "Usage request failed (HTTP " + std::to_string(res.statusCode) + ").";
    return out;
  }

  json r = json::parse(res.body, nullptr, false);
  if (r.is_discarded() || !r.is_object()) {
    out.error = "Usage response was not valid JSON.";
    return out;
  }
  if (r.contains("session_pct") && r["session_pct"].is_number()) {
    out.hasSession = true;
    out.sessionPct = r["session_pct"].get<double>();
  }
  out.sessionResetsAt = r.value("session_resets_at", "");
  if (r.contains("weekly_pct") && r["weekly_pct"].is_number()) {
    out.hasWeekly = true;
    out.weeklyPct = r["weekly_pct"].get<double>();
  }
  out.weeklyResetsAt = r.value("weekly_resets_at", "");
  out.ok = true;
  return out;
}
