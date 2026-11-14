#pragma once
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Platform.h"
#include "UsageClient.h"

// The Claude models this account can use, fetched from Anthropic's models list with the Claude CLI's login and kept
// in a cache file, so the list is ready at startup and still works offline.
class ModelCatalog
{
public:
    struct Model
    {
        std::string Id;
        std::string Name;       // "Opus 5.5"
        std::string Family;     // "opus"
        std::string CreatedAt;  // ISO 8601, so newer sorts after older
        std::int64_t MaxInputTokens = 0;  // 0 = not reported
    };

    explicit ModelCatalog(std::filesystem::path CachePath) : Shared_(std::make_shared<SharedState>()), CachePath_(std::move(CachePath))
    {
        LoadCache();
    }

    std::vector<Model> Snapshot() const
    {
        std::lock_guard<std::mutex> const Guard(Shared_->Mutex);
        return Shared_->Models.empty() == true ? BuiltIn() : Shared_->Models;
    }

    // Context window in tokens for a model id (dated ids match their undated form), or 0 when unknown.
    std::int64_t ContextWindowFor(const std::string& Id) const
    {
        return FindLocked(Id).MaxInputTokens;
    }

    // Id is empty in the returned copy when the model is not in the list.
    Model Find(const std::string& Id) const
    {
        return FindLocked(Id);
    }

    bool Knows(const std::string& Id) const
    {
        return FindLocked(Id).Id.empty() == false;
    }

    // Asks for a fresh list when a session reports a model that is not in it yet. Retries at most every 10 minutes, doubling after each failure up to about 5 hours.
    void RefreshIfUnknown(const std::string& Id, const std::function<void()>& Wake)
    {
        if (Id.empty() == true || Id.rfind("claude-", 0) != 0 || Knows(Id) == true)
        {
            return;
        }
        Refresh(true, Wake);
    }

    // Starts a background fetch when the list is older than a day (or when forced, at most every 10 minutes). Wake runs once it lands.
    void Refresh(const bool Force, const std::function<void()>& Wake)
    {
        constexpr std::time_t STALE_SECONDS = 24 * 60 * 60;
        constexpr std::time_t FORCE_COOLDOWN_SECONDS = 10 * 60;
        {
            std::lock_guard<std::mutex> const Guard(Shared_->Mutex);
            if (Shared_->InFlight == true)
            {
                return;
            }
            const std::time_t Current = std::time(nullptr);
            if (Force == false && Shared_->Models.empty() == false && Current - Shared_->FetchedAt < STALE_SECONDS)
            {
                return;
            }
            const int Doublings = std::min(Shared_->FailedAttempts, MAX_BACKOFF_DOUBLINGS);
            if (Force == true && Current - Shared_->LastAttemptAt < FORCE_COOLDOWN_SECONDS * (std::time_t(1) << Doublings))
            {
                return;
            }
            Shared_->LastAttemptAt = Current;
            Shared_->InFlight = true;
        }
        const std::shared_ptr<SharedState> Shared = Shared_;
        const std::filesystem::path CachePath = CachePath_;
        std::thread(
            [Shared, CachePath, Wake]
            {
                std::vector<Model> Fetched = Fetch();
                {
                    std::lock_guard<std::mutex> const Guard(Shared->Mutex);
                    Shared->InFlight = false;
                    if (Fetched.empty() == true)
                    {
                        Shared->FailedAttempts++;
                    }
                    else
                    {
                        Shared->FailedAttempts = 0;
                        Shared->Models = std::move(Fetched);
                        Shared->FetchedAt = std::time(nullptr);
                        SaveCache(CachePath, Shared->Models, Shared->FetchedAt);
                    }
                }
                if (Wake != nullptr)
                {
                    Wake();
                }
            })
            .detach();
    }

private:
    struct SharedState
    {
        std::mutex Mutex;
        std::vector<Model> Models;
        std::time_t FetchedAt = 0;
        std::time_t LastAttemptAt = 0;
        int FailedAttempts = 0;
        bool InFlight = false;
    };

    // Id is empty in the returned copy when nothing matches.
    Model FindLocked(const std::string& Id) const
    {
        {
            std::lock_guard<std::mutex> const Guard(Shared_->Mutex);
            const Model Fetched = BestMatch(Shared_->Models, Id);
            if (Fetched.Id.empty() == false)
            {
                return Fetched;
            }
        }
        return BestMatch(BuiltIn(), Id);
    }

    // Exact id wins; otherwise the longest listed id that the dated id starts with.
    static Model BestMatch(const std::vector<Model>& Models, const std::string& Id)
    {
        Model Prefix;
        for (const Model& Current : Models)
        {
            if (Current.Id == Id)
            {
                return Current;
            }
            if (Id.size() > Current.Id.size() && Current.Id.size() > Prefix.Id.size() && Id.rfind(Current.Id + "-", 0) == 0)
            {
                Prefix = Current;
            }
        }
        return Prefix;
    }

    // Used before the first successful fetch.
    static std::vector<Model> BuiltIn()
    {
        return {{"claude-haiku-5-5", "Haiku 5.5", "haiku", "2026-10-07T18:00:00Z"},
                {"claude-sonnet-5-5", "Sonnet 5.5", "sonnet", "2026-09-28T00:00:00Z"},
                {"claude-opus-5-5", "Opus 5.5", "opus", "2026-09-21T16:24:00Z"},
                {"claude-fable-5-1", "Fable 5.1", "fable", "2026-08-28T00:00:00Z"}};
    }

    static std::vector<Model> Fetch()
    {
        std::vector<Model> Found;
        const std::string Token = UsageClient::AccessToken();
        if (Token.empty() == true)
        {
            return Found;
        }
        std::string AfterId;
        for (int Page = 0; Page < MAX_PAGES; Page++)
        {
            const std::string Url = "https://api.anthropic.com/v1/models?limit=100" + (AfterId.empty() == true ? std::string() : "&after_id=" + AfterId);
            const Platform::HttpResult Result = Platform::HttpGetJson(Url, {
                                                                               {"Authorization", "Bearer " + Token},
                                                                               {"anthropic-beta", "oauth-2025-04-20"},
                                                                               {"anthropic-version", "2023-06-01"},
                                                                           });
            if (Result.Ok == false || Result.StatusCode != 200)
            {
                return {};
            }
            const nlohmann::json Response = nlohmann::json::parse(Result.Body, nullptr, false);
            if (Response.is_object() == false || Response.contains("data") == false || Response["data"].is_array() == false)
            {
                return {};
            }
            for (const nlohmann::json& Entry : Response["data"])
            {
                Model Parsed = FromJson(Entry, "id", "display_name", "created_at", "max_input_tokens");
                if (Parsed.Id.empty() == false)
                {
                    Found.push_back(std::move(Parsed));
                }
            }
            if (Response.value("has_more", false) == false || Response.contains("last_id") == false || Response["last_id"].is_string() == false)
            {
                break;
            }
            AfterId = Response["last_id"].get<std::string>();
        }
        std::sort(Found.begin(), Found.end(), [](const Model& Left, const Model& Right) { return Left.CreatedAt > Right.CreatedAt; });
        return Found;
    }

    static Model FromJson(const nlohmann::json& Entry, const char* const IdKey, const char* const NameKey, const char* const CreatedKey,
                          const char* const TokensKey)
    {
        Model Parsed;
        if (Entry.is_object() == false)
        {
            return Parsed;
        }
        Parsed.Id = StringField(Entry, IdKey);
        Parsed.Name = StringField(Entry, NameKey);
        Parsed.CreatedAt = StringField(Entry, CreatedKey);
        const nlohmann::json::const_iterator Tokens = Entry.find(TokensKey);
        if (Tokens != Entry.end() && Tokens->is_number_integer() == true)
        {
            Parsed.MaxInputTokens = Tokens->get<std::int64_t>();
        }
        const std::string Prefix = "Claude ";
        if (Parsed.Name.rfind(Prefix, 0) == 0)
        {
            Parsed.Name = Parsed.Name.substr(Prefix.size());
        }
        if (Parsed.Name.empty() == true)
        {
            Parsed.Name = Parsed.Id;
        }
        Parsed.Family = FamilyOf(Parsed.Id);
        return Parsed;
    }

    // "claude-opus-4-5-20251101" -> "opus"
    static std::string FamilyOf(const std::string& Id)
    {
        const std::string Prefix = "claude-";
        if (Id.rfind(Prefix, 0) != 0)
        {
            return Id;
        }
        const size_t End = Id.find('-', Prefix.size());
        return Id.substr(Prefix.size(), End == std::string::npos ? std::string::npos : End - Prefix.size());
    }

    static std::string StringField(const nlohmann::json& Entry, const char* const Key)
    {
        const nlohmann::json::const_iterator Found = Entry.find(Key);
        if (Found == Entry.end() || Found->is_string() == false)
        {
            return std::string();
        }
        return Found->get<std::string>();
    }

    void LoadCache()
    {
        std::ifstream Stream(CachePath_);
        if (Stream.good() == false)
        {
            return;
        }
        const nlohmann::json Root = nlohmann::json::parse(Stream, nullptr, false);
        if (Root.is_object() == false || Root.contains("models") == false || Root["models"].is_array() == false)
        {
            return;
        }
        std::vector<Model> Loaded;
        for (const nlohmann::json& Entry : Root["models"])
        {
            Model Parsed = FromJson(Entry, "id", "name", "createdAt", "maxInputTokens");
            if (Parsed.Id.empty() == false)
            {
                Loaded.push_back(std::move(Parsed));
            }
        }
        std::lock_guard<std::mutex> const Guard(Shared_->Mutex);
        Shared_->Models = std::move(Loaded);
        Shared_->FetchedAt = Root.contains("fetchedAt") == true && Root["fetchedAt"].is_number_integer() == true ? Root["fetchedAt"].get<std::time_t>() : 0;
    }

    static void SaveCache(const std::filesystem::path& Path, const std::vector<Model>& Models, const std::time_t FetchedAt)
    {
        nlohmann::json List = nlohmann::json::array();
        for (const Model& Current : Models)
        {
            List.push_back({{"id", Current.Id}, {"name", Current.Name}, {"createdAt", Current.CreatedAt}, {"maxInputTokens", Current.MaxInputTokens}});
        }
        std::error_code ErrorCode;
        std::filesystem::create_directories(Path.parent_path(), ErrorCode);
        const std::filesystem::path Temporary = Path.string() + ".tmp";
        {
            std::ofstream Stream(Temporary, std::ios::binary);
            Stream << nlohmann::json{{"fetchedAt", FetchedAt}, {"models", List}}.dump(2);
        }
        std::filesystem::rename(Temporary, Path, ErrorCode);
    }

    static constexpr int MAX_PAGES = 10;
    static constexpr int MAX_BACKOFF_DOUBLINGS = 5;

    std::shared_ptr<SharedState> Shared_;
    std::filesystem::path CachePath_;
};
