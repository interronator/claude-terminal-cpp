#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace util {
std::string uuid4();
std::vector<std::string> splitCommand(const std::string& cmd);
std::string utf8(uint32_t cp);
std::string trim(const std::string& s);
std::string slug(const std::string& s);
std::string baseName(const std::string& path);
}  // namespace util
