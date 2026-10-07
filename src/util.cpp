#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <random>

namespace util {

std::string uuid4() {
  static std::mt19937_64 rng{std::random_device{}()};
  uint64_t a = rng(), b = rng();
  a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
  b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // variant 10xx
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", (unsigned)(a >> 32), (unsigned)((a >> 16) & 0xFFFF),
                (unsigned)(a & 0xFFFF), (unsigned)(b >> 48), (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
  return buf;
}

// Splits on whitespace, honoring simple "double" and 'single' quotes.
std::vector<std::string> splitCommand(const std::string& cmd) {
  std::vector<std::string> out;
  std::string cur;
  bool inTok = false;
  char quote = 0;
  for (char c : cmd) {
    if (quote) {
      if (c == quote) quote = 0;
      else cur += c;
    } else if (c == '"' || c == '\'') {
      quote = c;
      inTok = true;
    } else if (std::isspace((unsigned char)c)) {
      if (inTok) out.push_back(cur), cur.clear(), inTok = false;
    } else {
      cur += c;
      inTok = true;
    }
  }
  if (inTok) out.push_back(cur);
  return out;
}

std::string utf8(uint32_t cp) {
  std::string s;
  if (cp < 0x80) s += (char)cp;
  else if (cp < 0x800) {
    s += (char)(0xC0 | (cp >> 6));
    s += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  } else {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string slug(const std::string& s) {
  std::string out;
  bool dash = false;
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out += (char)std::tolower(c);
      dash = false;
    } else if (!dash && !out.empty()) {
      out += '-';
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.size() > 40) out.resize(40);
  return out;
}

std::string baseName(const std::string& path) {
  std::filesystem::path p(path);
  if (p.filename().empty()) p = p.parent_path();
  std::string n = p.filename().string();
  return n.empty() ? path : n;
}

}  // namespace util
