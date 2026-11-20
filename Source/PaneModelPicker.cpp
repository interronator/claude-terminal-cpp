#include "App.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "SessionDefaults.h"

std::int64_t App::ContextWindowOf(const TranscriptScanner& Scanner) const
{
    return ModelCatalog_.ContextWindowFor(Scanner.CurrentModel());
}

std::string App::ModelLabelFor(const Session& CurrentSession) const
{
    // A model picked in the header wins over the transcript's model, which names the model that answered (opusplan swaps between two).
    const std::map<std::string, std::shared_ptr<TranscriptScanner>>::const_iterator Scanner = TranscriptScanners_.find(CurrentSession.Rec.Id);
    std::string ModelId = CurrentSession.Rec.Model;
    if (ModelId.empty() == true && Scanner != TranscriptScanners_.end())
    {
        ModelId = Scanner->second->CurrentModel();
    }
    if (ModelId.empty() == true)
    {
        ModelId = State_.AppSettings.DefaultModel;
    }
    if (ModelId.empty() == true)
    {
        return "Default";
    }
    if (ModelId == "opusplan")
    {
        return "Opus Plan";
    }
    if (ModelId == "opus")
    {
        return "Opus";
    }
    const ModelCatalog::Model Known = ModelCatalog_.Find(ModelId);
    if (Known.Id.empty() == false)
    {
        return Known.Name;
    }
    return ModelId;
}

float App::PaneModelPickerWidth(const Session& CurrentSession) const
{
    const ImGuiStyle& Style = ImGui::GetStyle();
    const float LabelWidth = ImGui::CalcTextSize(ModelLabelFor(CurrentSession).c_str()).x;
    return LabelWidth + Style.FramePadding.x * 2 + ImGui::GetFrameHeight();
}

void App::DrawPaneModelPicker(Session& CurrentSession, const float Width)
{
    const std::map<std::string, std::shared_ptr<TranscriptScanner>>::const_iterator Scanner = TranscriptScanners_.find(CurrentSession.Rec.Id);
    if (Scanner != TranscriptScanners_.end())
    {
        ModelCatalog_.RefreshIfUnknown(Scanner->second->CurrentModel(), Session::Wake);
    }

    std::vector<SessionDefaults::Choice> Latest;
    std::vector<SessionDefaults::Choice> Older;
    SessionDefaults::ModelChoices(ModelCatalog_.Snapshot(), Latest, Older);

    const std::string Label = ModelLabelFor(CurrentSession);
    ImGui::PushID("modelPicker");
    ImGui::SetNextItemWidth(Width);
    const bool Open = ImGui::BeginCombo("##model", Label.c_str()) == true;
    if (ImGui::IsItemHovered() == true && Open == false)
    {
        ImGui::SetTooltip("Model this session uses. Pick another to switch to it");
    }
    if (Open == true)
    {
        std::string Picked = CurrentSession.Rec.Model;
        if (SessionDefaults::DrawChoiceList(Picked, Latest, Older) == true)
        {
            CurrentSession.Rec.Model = Picked;
            if (CurrentSession.IsRunning() == true)
            {
                SendPrompt(CurrentSession, "/model " + (Picked.empty() == true ? std::string("default") : Picked));
            }
            MarkDirty();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void App::DrawContextMeter(const Session& CurrentSession, const float BarWidth, const float HeaderHeight)
{
    const std::map<std::string, std::shared_ptr<TranscriptScanner>>::const_iterator Scanner = TranscriptScanners_.find(CurrentSession.Rec.Id);
    if (Scanner == TranscriptScanners_.end())
    {
        return;
    }
    const std::int64_t Window = Scanner->second->ContextWindow(ContextWindowOf(*Scanner->second));
    const int Percent = Scanner->second->ContextPercent(ContextWindowOf(*Scanner->second));
    if (Percent < 0)
    {
        return;
    }
    const float Scale = UiScale_;
    const ImVec4 Color = ContextColor(Percent);
    const ImVec2 Origin = ImGui::GetCursorScreenPos();
    const std::string Text = (BarWidth > 0.0f ? "" : "ctx ") + std::to_string(Percent) + "%";
    const float TextWidth = ImGui::CalcTextSize(Text.c_str()).x;
    const float TotalWidth = BarWidth > 0.0f ? BarWidth + 6 * Scale + TextWidth : TextWidth;

    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    float TextX = Origin.x;
    if (BarWidth > 0.0f)
    {
        const float BarHeight = 6 * Scale;
        const float BarTop = Origin.y + (HeaderHeight - BarHeight) * 0.5f;
        const float Rounding = BarHeight * 0.5f;
        DrawList->AddRectFilled(ImVec2(Origin.x, BarTop), ImVec2(Origin.x + BarWidth, BarTop + BarHeight), PaneBorderColorU32_, Rounding);
        const float FillWidth = std::max(BarHeight, BarWidth * static_cast<float>(Percent) / 100.0f);
        DrawList->AddRectFilled(ImVec2(Origin.x, BarTop), ImVec2(Origin.x + FillWidth, BarTop + BarHeight), ImGui::GetColorU32(Color), Rounding);
        TextX = Origin.x + BarWidth + 6 * Scale;
    }
    DrawList->AddText(ImVec2(TextX, Origin.y + (HeaderHeight - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(Color), Text.c_str());

    ImGui::Dummy(ImVec2(TotalWidth, HeaderHeight));
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Context window in use: %s of %s tokens (%d%%)", FormatTokens(Scanner->second->ContextTokens()).c_str(), FormatTokens(Window).c_str(), Percent);
    }
}

std::string App::FormatTokens(const std::int64_t Tokens)
{
    constexpr std::int64_t MILLION = 1000000;
    constexpr std::int64_t THOUSAND = 1000;
    if (Tokens >= MILLION && Tokens % MILLION == 0)
    {
        return std::to_string(Tokens / MILLION) + "M";
    }
    if (Tokens >= THOUSAND)
    {
        return std::to_string(Tokens / THOUSAND) + "k";
    }
    return std::to_string(Tokens);
}
