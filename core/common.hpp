#pragma once
#include <dafeiyu/core.hpp>
#include <dafeiyu/schema.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace dfy {
using SteadyClock = std::chrono::steady_clock;
int64_t now_ms();
std::string identifier();
std::string fingerprint(std::string_view text);
std::string read_text(const Path& path);
Json read_json(const Path& path);
void require(bool condition, const std::string& code);
bool includes(const Json& values, const Json& value);
bool terminal(const Json& run);
Json failure(const std::exception& error);
void validate_outcome(const Json& value);
}
