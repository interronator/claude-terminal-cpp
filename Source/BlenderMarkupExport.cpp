#include "BlenderMarkupExport.h"

#include <algorithm>
#include <cmath>

#include "stb_image.h"
#include "stb_image_write.h"

namespace
{
constexpr int GLYPH_WIDTH = 5;
constexpr int GLYPH_HEIGHT = 7;

const char* const DIGIT_ROWS[10][GLYPH_HEIGHT] = {
    {"01110", "10001", "10011", "10101", "11001", "10001", "01110"}, {"00100", "01100", "00100", "00100", "00100", "00100", "01110"},
    {"01110", "10001", "00001", "00010", "00100", "01000", "11111"}, {"11110", "00001", "00001", "01110", "00001", "00001", "11110"},
    {"00010", "00110", "01010", "10010", "11111", "00010", "00010"}, {"11111", "10000", "11110", "00001", "00001", "10001", "01110"},
    {"00110", "01000", "10000", "11110", "10001", "10001", "01110"}, {"11111", "00001", "00010", "00100", "01000", "01000", "01000"},
    {"01110", "10001", "10001", "01110", "10001", "10001", "01110"}, {"01110", "10001", "10001", "01111", "00001", "00010", "01100"}};
}  // namespace

void BlenderMarkupExport::PutPixel(const Canvas& Target, const int PosX, const int PosY, const ImU32 Color)
{
    if (PosX < 0 || PosY < 0 || PosX >= Target.Width || PosY >= Target.Height)
    {
        return;
    }
    unsigned char* const Pixel = Target.Pixels + (static_cast<size_t>(PosY) * static_cast<size_t>(Target.Width) + static_cast<size_t>(PosX)) * 4;
    Pixel[0] = static_cast<unsigned char>(Color & 0xFF);
    Pixel[1] = static_cast<unsigned char>((Color >> 8) & 0xFF);
    Pixel[2] = static_cast<unsigned char>((Color >> 16) & 0xFF);
    Pixel[3] = 255;
}

void BlenderMarkupExport::FillDisc(const Canvas& Target, const float CenterX, const float CenterY, const float Radius, const ImU32 Color)
{
    const int Reach = static_cast<int>(std::ceil(Radius));
    const int CenterPixelX = static_cast<int>(std::lround(CenterX));
    const int CenterPixelY = static_cast<int>(std::lround(CenterY));
    for (int OffsetY = -Reach; OffsetY <= Reach; OffsetY++)
    {
        for (int OffsetX = -Reach; OffsetX <= Reach; OffsetX++)
        {
            if (static_cast<float>(OffsetX * OffsetX + OffsetY * OffsetY) > Radius * Radius)
            {
                continue;
            }
            PutPixel(Target, CenterPixelX + OffsetX, CenterPixelY + OffsetY, Color);
        }
    }
}

void BlenderMarkupExport::DrawLine(const Canvas& Target, const ImVec2 From, const ImVec2 To, const float Radius, const ImU32 Color)
{
    const float Length = std::hypot(To.x - From.x, To.y - From.y);
    const int Steps = std::max(1, static_cast<int>(Length / std::max(Radius * 0.5f, 1.0f)));
    for (int Step = 0; Step <= Steps; Step++)
    {
        const float Along = static_cast<float>(Step) / static_cast<float>(Steps);
        FillDisc(Target, From.x + (To.x - From.x) * Along, From.y + (To.y - From.y) * Along, Radius, Color);
    }
}

void BlenderMarkupExport::DrawBadge(const Canvas& Target, const ImVec2 Center, const float Radius, const int Number, const ImU32 Color)
{
    const std::string Text = std::to_string(Number);
    const int Cell = std::max(1, static_cast<int>(Radius / 4.5f));
    const int Gap = Cell;
    const int TextWidth = static_cast<int>(Text.size()) * GLYPH_WIDTH * Cell + (static_cast<int>(Text.size()) - 1) * Gap;
    const float Stretch = std::max(0.0f, static_cast<float>(TextWidth) * 0.5f + static_cast<float>(Cell) - Radius);
    DrawLine(Target, ImVec2(Center.x - Stretch, Center.y), ImVec2(Center.x + Stretch, Center.y), Radius, Color);
    const int StartX = static_cast<int>(Center.x) - TextWidth / 2;
    const int StartY = static_cast<int>(Center.y) - GLYPH_HEIGHT * Cell / 2;
    for (size_t Digit = 0; Digit < Text.size(); Digit++)
    {
        const int Glyph = Text[Digit] - '0';
        const int GlyphX = StartX + static_cast<int>(Digit) * (GLYPH_WIDTH + 1) * Cell;
        for (int Row = 0; Row < GLYPH_HEIGHT; Row++)
        {
            for (int Column = 0; Column < GLYPH_WIDTH; Column++)
            {
                if (DIGIT_ROWS[Glyph][Row][Column] != '1')
                {
                    continue;
                }
                for (int BlockY = 0; BlockY < Cell; BlockY++)
                {
                    for (int BlockX = 0; BlockX < Cell; BlockX++)
                    {
                        PutPixel(Target, GlyphX + Column * Cell + BlockX, StartY + Row * Cell + BlockY, IM_COL32(0, 0, 0, 255));
                    }
                }
            }
        }
    }
}

void BlenderMarkupExport::DrawMark(const Canvas& Target, const BlenderMarkup::Mark& Item, const int Number, const ImVec2 SourceSize, const ImVec2 Offset,
                                   const float Zoom, const float Thickness, const float BadgeRadius)
{
    constexpr int ELLIPSE_SEGMENTS = 64;
    constexpr float TWO_PI = 6.28318530718f;
    if (Item.Points.size() < 2)
    {
        return;
    }
    std::vector<ImVec2> Points;
    Points.reserve(Item.Points.size());
    for (const ImVec2& Point : Item.Points)
    {
        Points.push_back(ImVec2((Point.x * SourceSize.x - Offset.x) * Zoom, (Point.y * SourceSize.y - Offset.y) * Zoom));
    }
    ImVec2 Anchor = Points[0];
    if (Item.Kind == BlenderMarkup::Tool::Pen)
    {
        for (size_t Segment = 1; Segment < Points.size(); Segment++)
        {
            DrawLine(Target, Points[Segment - 1], Points[Segment], Thickness, Item.Color);
        }
    }
    else
    {
        const ImVec2 Low(std::min(Points[0].x, Points[1].x), std::min(Points[0].y, Points[1].y));
        const ImVec2 High(std::max(Points[0].x, Points[1].x), std::max(Points[0].y, Points[1].y));
        if (Item.Kind == BlenderMarkup::Tool::Rectangle)
        {
            DrawLine(Target, Low, ImVec2(High.x, Low.y), Thickness, Item.Color);
            DrawLine(Target, ImVec2(High.x, Low.y), High, Thickness, Item.Color);
            DrawLine(Target, High, ImVec2(Low.x, High.y), Thickness, Item.Color);
            DrawLine(Target, ImVec2(Low.x, High.y), Low, Thickness, Item.Color);
        }
        else
        {
            const ImVec2 Center((Low.x + High.x) * 0.5f, (Low.y + High.y) * 0.5f);
            const ImVec2 Radius((High.x - Low.x) * 0.5f, (High.y - Low.y) * 0.5f);
            ImVec2 Previous(Center.x + Radius.x, Center.y);
            for (int Step = 1; Step <= ELLIPSE_SEGMENTS; Step++)
            {
                const float Angle = TWO_PI * static_cast<float>(Step) / static_cast<float>(ELLIPSE_SEGMENTS);
                const ImVec2 Next(Center.x + std::cos(Angle) * Radius.x, Center.y + std::sin(Angle) * Radius.y);
                DrawLine(Target, Previous, Next, Thickness, Item.Color);
                Previous = Next;
            }
        }
        Anchor = Low;
    }
    const float FullWidth = static_cast<float>(Target.Width);
    const float FullHeight = static_cast<float>(Target.Height);
    const ImVec2 Badge(std::clamp(Anchor.x, BadgeRadius, FullWidth - BadgeRadius), std::clamp(Anchor.y, BadgeRadius, FullHeight - BadgeRadius));
    DrawBadge(Target, Badge, BadgeRadius, Number, Item.Color);
}

bool BlenderMarkupExport::Save(const std::string& SourcePath, const std::vector<BlenderMarkup::Mark>& Marks, const int FirstNumber, const std::string& OutputPath,
                               std::string& Error)
{
    int Width = 0;
    int Height = 0;
    int Channels = 0;
    unsigned char* const Pixels = stbi_load(SourcePath.c_str(), &Width, &Height, &Channels, 4);
    if (Pixels == nullptr)
    {
        Error = "Could not read the picture " + SourcePath;
        return false;
    }
    const Canvas Target{Pixels, Width, Height};
    const float Thickness = std::max(2.0f, static_cast<float>(Width) / 400.0f);
    const float BadgeRadius = std::max(9.0f, static_cast<float>(Width) / 90.0f);
    const ImVec2 SourceSize(static_cast<float>(Width), static_cast<float>(Height));
    for (size_t Index = 0; Index < Marks.size(); Index++)
    {
        DrawMark(Target, Marks[Index], FirstNumber + static_cast<int>(Index), SourceSize, ImVec2(0.0f, 0.0f), 1.0f, Thickness, BadgeRadius);
    }
    const int Written = stbi_write_png(OutputPath.c_str(), Width, Height, 4, Pixels, Width * 4);
    stbi_image_free(Pixels);
    if (Written == 0)
    {
        Error = "Could not write the marked picture " + OutputPath;
        return false;
    }
    return true;
}

bool BlenderMarkupExport::SaveCrop(const std::string& SourcePath, const BlenderMarkup::Mark& Item, const int Number, const std::string& OutputPath,
                                   std::string& Error)
{
    constexpr float MARGIN_FRACTION = 0.3f;
    constexpr float MARGIN_MIN_PIXELS = 16.0f;
    constexpr float CROP_MIN_PIXELS = 160.0f;
    constexpr float TARGET_LONG_SIDE = 640.0f;
    if (Item.Points.size() < 2)
    {
        Error = "The mark has no area to crop";
        return false;
    }
    int Width = 0;
    int Height = 0;
    int Channels = 0;
    unsigned char* const Pixels = stbi_load(SourcePath.c_str(), &Width, &Height, &Channels, 4);
    if (Pixels == nullptr)
    {
        Error = "Could not read the picture " + SourcePath;
        return false;
    }
    const float FullWidth = static_cast<float>(Width);
    const float FullHeight = static_cast<float>(Height);
    ImVec2 Low(FullWidth, FullHeight);
    ImVec2 High(0.0f, 0.0f);
    for (const ImVec2& Point : Item.Points)
    {
        Low.x = std::min(Low.x, Point.x * FullWidth);
        Low.y = std::min(Low.y, Point.y * FullHeight);
        High.x = std::max(High.x, Point.x * FullWidth);
        High.y = std::max(High.y, Point.y * FullHeight);
    }
    const float Pad = std::max(High.x - Low.x, High.y - Low.y) * MARGIN_FRACTION + MARGIN_MIN_PIXELS;
    const float CropWidth = std::min(FullWidth, std::max(CROP_MIN_PIXELS, High.x - Low.x + 2.0f * Pad));
    const float CropHeight = std::min(FullHeight, std::max(CROP_MIN_PIXELS, High.y - Low.y + 2.0f * Pad));
    const float CropLeft = std::clamp((Low.x + High.x) * 0.5f - CropWidth * 0.5f, 0.0f, FullWidth - CropWidth);
    const float CropTop = std::clamp((Low.y + High.y) * 0.5f - CropHeight * 0.5f, 0.0f, FullHeight - CropHeight);
    const int SourceLeft = static_cast<int>(std::floor(CropLeft));
    const int SourceTop = static_cast<int>(std::floor(CropTop));
    const int SourceWidth = std::clamp(static_cast<int>(std::ceil(CropWidth)), 1, Width - SourceLeft);
    const int SourceHeight = std::clamp(static_cast<int>(std::ceil(CropHeight)), 1, Height - SourceTop);
    const float Zoom = std::max(1.0f, TARGET_LONG_SIDE / static_cast<float>(std::max(SourceWidth, SourceHeight)));
    const int OutWidth = std::max(1, static_cast<int>(std::lround(static_cast<float>(SourceWidth) * Zoom)));
    const int OutHeight = std::max(1, static_cast<int>(std::lround(static_cast<float>(SourceHeight) * Zoom)));

    std::vector<unsigned char> Out(static_cast<size_t>(OutWidth) * static_cast<size_t>(OutHeight) * 4);
    for (int OutY = 0; OutY < OutHeight; OutY++)
    {
        const float SourceY = std::clamp(static_cast<float>(SourceTop) + (static_cast<float>(OutY) + 0.5f) / Zoom - 0.5f, 0.0f, FullHeight - 1.0f);
        const int Row0 = static_cast<int>(std::floor(SourceY));
        const int Row1 = std::min(Row0 + 1, Height - 1);
        const float BlendY = SourceY - static_cast<float>(Row0);
        for (int OutX = 0; OutX < OutWidth; OutX++)
        {
            const float SourceX = std::clamp(static_cast<float>(SourceLeft) + (static_cast<float>(OutX) + 0.5f) / Zoom - 0.5f, 0.0f, FullWidth - 1.0f);
            const int Column0 = static_cast<int>(std::floor(SourceX));
            const int Column1 = std::min(Column0 + 1, Width - 1);
            const float BlendX = SourceX - static_cast<float>(Column0);
            for (int Channel = 0; Channel < 4; Channel++)
            {
                const float TopMix = static_cast<float>(Pixels[(static_cast<size_t>(Row0) * Width + Column0) * 4 + Channel]) * (1.0f - BlendX) +
                                     static_cast<float>(Pixels[(static_cast<size_t>(Row0) * Width + Column1) * 4 + Channel]) * BlendX;
                const float BottomMix = static_cast<float>(Pixels[(static_cast<size_t>(Row1) * Width + Column0) * 4 + Channel]) * (1.0f - BlendX) +
                                        static_cast<float>(Pixels[(static_cast<size_t>(Row1) * Width + Column1) * 4 + Channel]) * BlendX;
                Out[(static_cast<size_t>(OutY) * OutWidth + OutX) * 4 + Channel] = static_cast<unsigned char>(TopMix * (1.0f - BlendY) + BottomMix * BlendY + 0.5f);
            }
        }
    }
    stbi_image_free(Pixels);

    const Canvas Target{Out.data(), OutWidth, OutHeight};
    const float Thickness = std::max(2.0f, static_cast<float>(OutWidth) / 200.0f);
    const float BadgeRadius = std::max(9.0f, static_cast<float>(OutWidth) / 40.0f);
    DrawMark(Target, Item, Number, ImVec2(FullWidth, FullHeight), ImVec2(static_cast<float>(SourceLeft), static_cast<float>(SourceTop)), Zoom, Thickness,
             BadgeRadius);
    if (stbi_write_png(OutputPath.c_str(), OutWidth, OutHeight, 4, Out.data(), OutWidth * 4) == 0)
    {
        Error = "Could not write the zoomed picture " + OutputPath;
        return false;
    }
    return true;
}
