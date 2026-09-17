#pragma once
namespace iris {
// Parse a complete, prevalidated numeric token; hex input excludes its sign and 0x prefix.
bool parse_float(const char* first, const char* last, float& value, bool hex);
} // namespace iris
