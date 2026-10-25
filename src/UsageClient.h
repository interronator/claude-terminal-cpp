#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "Platform.h"

struct UsageInfo
{
    bool Ok = false;
    std::string Error;
    bool HasSession = false;
    double SessionPct = 0;
    std::string SessionResetsAt;
    bool HasWeekly = false;
    double WeeklyPct = 0;
    std::string WeeklyResetsAt;
};

class UsageClient
{
public:
    // Blocking: reads the Claude CLI's stored OAuth token and calls the (undocumented) subscription
    // usage endpoint. Call from a background thread only.
    static UsageInfo Fetch()
    {
        UsageInfo Out;
        std::ifstream In(CredentialsPath());
        if (In.good() == false)
        {
            Out.Error = NOT_LOGGED_IN;
            return Out;
        }
        nlohmann::json Credentials = nlohmann::json::parse(In, nullptr, /*allow_exceptions=*/false);
        std::string Token;
        if (Credentials.is_discarded() == false && Credentials.is_object() == true && Credentials.contains("claudeAiOauth") == true &&
            Credentials["claudeAiOauth"].is_object() == true)
        {
            const nlohmann::json& OauthJson = Credentials["claudeAiOauth"];
            if (OauthJson.contains("accessToken") == true && OauthJson["accessToken"].is_string() == true)
            {
                Token = OauthJson["accessToken"].get<std::string>();
            }
        }
        if (Token.empty())
        {
            Out.Error = NOT_LOGGED_IN;
            return Out;
        }

        Platform::HttpResult const Result = Platform::HttpGetJson("https://api.anthropic.com/api/oauth/usage",
                                                             {
                                                                 {"Authorization", "Bearer " + Token},
                                                                 {"anthropic-beta", "oauth-2025-04-20"},
                                                                 {"Content-Type", "application/json"},
                                                             });
        if (Result.Ok == false)
        {
            Out.Error = Result.Error.empty() ? "Could not reach the usage endpoint." : Result.Error;
            return Out;
        }
        if (Result.StatusCode == 401)
        {
            Out.Error = "Could not fetch usage - try running a Claude session first to refresh your login.";
            return Out;
        }
        if (Result.StatusCode != 200)
        {
            Out.Error = "Usage request failed (HTTP " + std::to_string(Result.StatusCode) + ").";
            return Out;
        }

        nlohmann::json const Response = nlohmann::json::parse(Result.Body, nullptr, false);
        if (Response.is_discarded() || Response.is_object() == false)
        {
            Out.Error = "Usage response was not valid JSON.";
            return Out;
        }
        // The real response nests each window as {"utilization": <percent>, "resets_at": <iso8601>}
        // under "five_hour" (the rolling session window) and "seven_day" (the rolling weekly window) -
        // not the flat session_pct/weekly_pct fields this used to look for.
        ReadBucket(Response, "five_hour", Out.HasSession, Out.SessionPct, Out.SessionResetsAt);
        ReadBucket(Response, "seven_day", Out.HasWeekly, Out.WeeklyPct, Out.WeeklyResetsAt);
        Out.Ok = true;
        return Out;
    }

private:
    static void ReadBucket(const nlohmann::json& Response, const char* Key, bool& HasValue, double& Pct, std::string& ResetsAt)
    {
        if (Response.contains(Key) == false || Response[Key].is_object() == false)
        {
            return;
        }
        const nlohmann::json& Bucket = Response[Key];
        if (Bucket.contains("utilization") == true && Bucket["utilization"].is_number() == true)
        {
            HasValue = true;
            Pct = Bucket["utilization"].get<double>();
        }
        if (Bucket.contains("resets_at") == true && Bucket["resets_at"].is_string() == true)
        {
            ResetsAt = Bucket["resets_at"].get<std::string>();
        }
    }

    static constexpr const char* NOT_LOGGED_IN = "Not logged in to Claude - run `claude` once to sign in, then try again.";

    static std::string CredentialsPath()
    {
        if (const char* const ConfigDir = std::getenv("CLAUDE_CONFIG_DIR"))
        {
            return (std::filesystem::path(ConfigDir) / ".credentials.json").string();
        }
#ifdef _WIN32
        const char* const Home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        std::filesystem::path const Dir = Home != nullptr ? std::filesystem::path(Home) : std::filesystem::path(".");
        return (Dir / ".claude" / ".credentials.json").string();
    }
};
