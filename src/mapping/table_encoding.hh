// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace surge_mm
{
inline std::string table_base64(const uint8_t *p, size_t size) {
	constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	result.reserve((size + 2) / 3 * 4);
	for (size_t i = 0; i < size; i += 3) {
		uint32_t x = uint32_t(p[i]) << 16;
		if (i + 1 < size)
			x |= uint32_t(p[i + 1]) << 8;
		if (i + 2 < size)
			x |= p[i + 2];
		result += alphabet[(x >> 18) & 63];
		result += alphabet[(x >> 12) & 63];
		result += i + 1 < size ? alphabet[(x >> 6) & 63] : '=';
		result += i + 2 < size ? alphabet[x & 63] : '=';
	}
	return result;
}
inline bool table_unbase64(std::string_view s, std::vector<uint8_t> &out, size_t limit) {
	auto digit = [](char c) -> int {
		if (c >= 'A' && c <= 'Z')
			return c - 'A';
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 26;
		if (c >= '0' && c <= '9')
			return c - '0' + 52;
		return c == '+' ? 62 : c == '/' ? 63 : -1;
	};
	if (s.size() % 4 || s.size() / 4 * 3 > limit + 2)
		return false;
	out.clear();
	out.reserve(s.size() / 4 * 3);
	for (size_t i = 0; i < s.size(); i += 4) {
		int a = digit(s[i]), b = digit(s[i + 1]), c = s[i + 2] == '=' ? 0 : digit(s[i + 2]),
			d = s[i + 3] == '=' ? 0 : digit(s[i + 3]);
		bool pad2 = s[i + 2] == '=', pad1 = s[i + 3] == '=';
		if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad1) || ((pad1 || pad2) && i + 4 != s.size()) ||
			(pad2 && (b & 15)) || (!pad2 && pad1 && (c & 3)))
			return false;
		uint32_t x = (a << 18) | (b << 12) | (c << 6) | d;
		out.push_back(x >> 16);
		if (!pad2)
			out.push_back(x >> 8);
		if (!pad1)
			out.push_back(x);
	}
	return out.size() <= limit;
}
} // namespace surge_mm