// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "patch/patch_data.hh"
#include <stdexcept>
#include <string>
#include <vector>

namespace SurgeNativeExport
{
struct Error : std::runtime_error {
	using std::runtime_error::runtime_error;
};
struct Report {
	unsigned converted_modules{}, helper_modules{};
	std::vector<std::string> notes;
};
// Input is the complete Hub export: parameters and mapping ranges are already
// normalized to their ORIGINAL Rack ranges. Existing IDs remain stable; helpers
// are appended. On any failure the caller's patch is unchanged.
Report convert(MetaModule::PatchData &patch);
} // namespace SurgeNativeExport
