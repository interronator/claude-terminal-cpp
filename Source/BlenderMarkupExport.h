#pragma once
#include <string>
#include <vector>

#include "BlenderMarkup.h"

// Burns the user's marks (outlines, strokes and number badges) into a copy of a picture, so Claude can see them.
class BlenderMarkupExport
{
public:
    // Reads SourcePath, draws the marks on it (badges numbered from FirstNumber) and writes the result as a PNG to OutputPath.
    // Returns false and sets Error on failure.
    static bool Save(const std::string& SourcePath, const std::vector<BlenderMarkup::Mark>& Marks, const int FirstNumber, const std::string& OutputPath,
                     std::string& Error);

    // Writes an enlarged crop of the (unmarked) picture around one mark, with only that mark and its number drawn on it.
    static bool SaveCrop(const std::string& SourcePath, const BlenderMarkup::Mark& Item, const int Number, const std::string& OutputPath, std::string& Error);

private:
    struct Canvas
    {
        unsigned char* Pixels = nullptr;
        int Width = 0;
        int Height = 0;
    };

    static void PutPixel(const Canvas& Target, const int PosX, const int PosY, const ImU32 Color);
    static void FillDisc(const Canvas& Target, const float CenterX, const float CenterY, const float Radius, const ImU32 Color);
    static void DrawLine(const Canvas& Target, const ImVec2 From, const ImVec2 To, const float Radius, const ImU32 Color);
    static void DrawBadge(const Canvas& Target, const ImVec2 Center, const float Radius, const int Number, const ImU32 Color);
    static void DrawMark(const Canvas& Target, const BlenderMarkup::Mark& Item, const int Number, const ImVec2 SourceSize, const ImVec2 Offset, const float Zoom,
                         const float Thickness, const float BadgeRadius);
};
