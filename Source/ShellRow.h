#pragma once

#include <string>

#include "imgui.h"

struct ShellRow
{
    std::string Kind;
    std::string Title;
    std::string Elapsed;
    std::string Hint;
    float Fraction = 0;
    bool Indeterminate = false;
    ImU32 Fill = 0;
};
