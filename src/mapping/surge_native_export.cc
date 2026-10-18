// SPDX-License-Identifier: GPL-3.0-or-later
#include "surge_native_export.hh"
#include "table_encoding.hh"
#include <jansson.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace SurgeNativeExport
{
namespace
{
using MetaModule::PatchData;
using Key = std::pair<unsigned, unsigned>;
void require(bool ok, std::string const &message) {
	if (!ok)
		throw Error(message);
}
float bounded(float value, float lo = 0, float hi = 1) {
	require(std::isfinite(value) && value >= lo && value <= hi, "Invalid or unrepresentable Surge parameter/range");
	return value;
}
unsigned rounded(float v) {
	return unsigned(std::max(0.f, std::floor(v + .5f)));
}
using Json = std::unique_ptr<json_t, decltype(&json_decref)>;
void allowed(json_t *object, std::initializer_list<std::string_view> keys) {
	require(json_is_object(object), "Surge saved state must be a JSON object");
	const char *key;
	json_t *value;
	json_object_foreach(object, key, value) {
		require(std::find(keys.begin(), keys.end(), key) != keys.end(),
				"Unsupported Surge saved state: " + std::string(key));
	}
}
float json_number(json_t *object, char const *key, float fallback) {
	auto *v = json_object_get(object, key);
	if (!v)
		return fallback;
	require(json_is_number(v), "Expected a number for " + std::string(key));
	return bounded(float(json_number_value(v)));
}
bool json_bool(json_t *object, char const *key, bool fallback = false) {
	auto *v = json_object_get(object, key);
	if (!v)
		return fallback;
	require(json_is_boolean(v), "Expected a boolean for " + std::string(key));
	return json_is_true(v);
}
std::string table_state(json_t *specific, unsigned order, bool steep, bool dc) {
	auto *table = json_object_get(specific, "wavetable");
	require(table, "Wavetable/Window export requires the saved wavetable data");
	allowed(table, {"draw3D", "display_name", "n_tables", "n_samples", "flags", "data"});
	json_bool(table, "draw3D");
	for (auto key : {"n_tables", "n_samples", "flags"})
		if (auto *v = json_object_get(table, key))
			require(json_is_integer(v) && json_integer_value(v) >= 0, "Invalid wavetable metadata");
	auto *encoded = json_object_get(table, "data");
	require(json_is_string(encoded), "Missing wavetable sample data");
	std::vector<uint8_t> bytes;
	require(surge_mm::table_unbase64(
				{json_string_value(encoded), json_string_length(encoded)}, bytes, 12 + 4096 * 512 * 4) &&
				bytes.size() >= 12,
			"Invalid wavetable base64/header");
	const auto word = [&](unsigned p) {
		return unsigned(bytes[p]) | unsigned(bytes[p + 1]) << 8;
	};
	const unsigned samples = word(4) | (word(6) << 16), frames = word(8), flags = word(10);
	require(samples >= 2 && samples <= 4096 && !(samples & (samples - 1)) && frames >= 1 && frames <= 512,
			"Wavetable dimensions exceed native support");
	require(!(flags & ~15u) && (!(flags & 8) || (flags & 4)), "Unsupported wavetable sample flags");
	const unsigned stride = flags & 4 ? 2 : 4;
	require(bytes.size() == 12 + size_t(samples) * frames * stride, "Incomplete or trailing wavetable sample data");
	require((bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0) ||
				std::memcmp(bytes.data(), "vawt", 4) == 0,
			"Unknown Rack wavetable header");
	if (!(flags & 4))
		for (size_t offset = 12; offset < bytes.size(); offset += 4) {
			uint32_t bits = unsigned(bytes[offset]) | unsigned(bytes[offset + 1]) << 8 |
							unsigned(bytes[offset + 2]) << 16 | unsigned(bytes[offset + 3]) << 24;
			float value;
			std::memcpy(&value, &bits, 4);
			require(std::isfinite(value) && std::abs(value) < 131071.f, "Invalid floating-point wavetable sample");
		}
	// Rack's embedded header deliberately has a zero tag. The samples/flags
	// are already a Surge WT payload; only the native loader's magic differs.
	std::memcpy(bytes.data(), "vawt", 4);
	std::string title = "Rack wavetable";
	if (auto *name = json_object_get(table, "display_name")) {
		require(json_is_string(name) && json_string_length(name) <= 1024, "Invalid wavetable display name");
		title.assign(json_string_value(name), json_string_length(name));
	}
	return std::string("SWT2:") + char('0' + order) + char('0' + steep) + char('0' + dc) +
		   "1:0:" + surge_mm::table_base64(reinterpret_cast<const uint8_t *>(title.data()), title.size()) + ":" +
		   surge_mm::table_base64(bytes.data(), bytes.size());
}
// Rack FX engine slots are not necessarily the native panel control order.
struct EffectSpec {
	char const *name;
	unsigned count, poly_param;
	int clock_param, clock_input;
	std::vector<unsigned> switches, integers;
	std::vector<float> gains;
	std::vector<std::pair<unsigned, float>> extra;
	unsigned outputs = 2;
	bool poly_default = false;
	std::vector<std::pair<unsigned, unsigned>> input_routes;
	std::vector<unsigned> booleans, overridden;
	// Empty means the original engine index is the native control index.
	// -1 is an unused source slot, with no audible native destination.
	std::vector<int> parameter_ids, cv_ids;
};
EffectSpec const *effect_spec(std::string const &model) {
	// IDs and switch order: src/FX.h and src/fxconfig/*.h. Native CV
	// coefficients: metamodule/dsp/*_dsp.cpp. Gain = Rack coefficient / native.
	static const EffectSpec specs[]{
		{.name = "Chorus",
		 .count = 8,
		 .poly_param = 10,
		 .clock_param = 11,
		 .clock_input = 10,
		 .switches = {8, 9},
		 .integers = {},
		 .gains = {.5, .5, .5, .5, 13.f / 12, 13.f / 12, .5, .5},
		 .extra = {}},
		{.name = "Flanger",
		 .count = 11,
		 .poly_param = 11,
		 .clock_param = 12,
		 .clock_input = 13,
		 .switches = {},
		 .integers = {0, 1},
		 .gains = {.5, .5, .5, .5, .5, 127.f / 120, .5, .5, .5, .5, .5},
		 .extra = {}},
		{.name = "Phaser",
		 .count = 12,
		 .poly_param = 14,
		 .clock_param = 15,
		 .clock_input = 14,
		 .switches = {12},
		 .integers = {8, 10},
		 .gains = {},
		 .extra = {{13, 1}}},
		{.name = "RotarySpeaker",
		 .count = 8,
		 .poly_param = 9,
		 .clock_param = 10,
		 .clock_input = 10,
		 .switches = {8},
		 .integers = {5},
		 .gains = {},
		 .extra = {}},
		{.name = "FrequencyShifter",
		 .count = 5,
		 .poly_param = 6,
		 .clock_param = 7,
		 .clock_input = 7,
		 .switches = {5},
		 .integers = {},
		 .gains = {},
		 .extra = {}},
		{.name = "Exciter",
		 .count = 5,
		 .poly_param = 5,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {},
		 .integers = {},
		 .gains = {},
		 .extra = {}},
		// Rack does not expose tempo sync on Reverb2; native's added clock
		// control/input must remain disabled for an imported Rack patch.
		{.name = "Reverb2",
		 .count = 10,
		 .poly_param = 10,
		 .clock_param = 11,
		 .clock_input = -1,
		 .switches = {},
		 .integers = {},
		 .gains = {},
		 .extra = {}},
		{.name = "Reverb",
		 .count = 11,
		 .poly_param = 13,
		 .clock_param = 14,
		 .clock_input = -1,
		 .switches = {11, 12},
		 .integers = {1},
		 .gains = {},
		 .extra = {}},
		{.name = "TreeMonster",
		 .count = 8,
		 .poly_param = 10,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {8, 9},
		 .integers = {},
		 .gains = {.5, .5, 13.f / 12, 13.f / 12, 1, .5, .5, .5},
		 .extra = {},
		 .outputs = 4,
		 .poly_default = true},
		{.name = "Neuron",
		 .count = 12,
		 .poly_param = 13,
		 .clock_param = 14,
		 .clock_input = 14,
		 .switches = {},
		 .integers = {7},
		 .gains = {.5, .5, .5, .5, .5, 13.f / 12, 1.6, .5, .5, .5, .5, .5},
		 .extra = {{12, 1}}},
		{.name = "Distortion",
		 .count = 12,
		 .poly_param = 14,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {12, 13},
		 .integers = {11},
		 .gains = {.5, 13.f / 12, .5, 13.f / 12, .5, .5, .5, 13.f / 12, .5, 13.f / 12, .5, .5},
		 .extra = {{15, 0}, {16, 0}, {17, 0}}},
		{.name = "RingMod",
		 .count = 9,
		 .poly_param = 11,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {9, 10},
		 .integers = {0, 3},
		 .gains = {.5, 127.f / 120, .5, .5, .5, .5, 13.f / 12, 13.f / 12, .5},
		 .extra = {{12, 0}, {13, 0}},
		 .outputs = 2,
		 .poly_default = false,
		 .input_routes = {{2, 11}, {3, 12}}},
		{.name = "Chow",
		 .count = 4,
		 .poly_param = 4,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {2},
		 .integers = {},
		 .gains = {},
		 .extra = {},
		 .outputs = 2,
		 .poly_default = false,
		 .input_routes = {{9, 4}},
		 .booleans = {2},
		 .overridden = {2}},
		{.name = "Bonsai",
		 .count = 11,
		 .poly_param = 11,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {},
		 .integers = {3, 4},
		 .gains = {},
		 .extra = {{12, 0}, {13, 0}}},
		{.name = "Combulator",
		 .count = 12,
		 .poly_param = 15,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {12},
		 .integers = {},
		 .gains = {.5, 187.3763f / 120, 1, 1, .5, .5, .5, .5, .5, .5, .5, .5},
		 .extra = {{13, 0}, {14, 0}}},
		// Rack only processes slots 0..10. Its hidden reconstruction filter
		// stays at the engine default even when preset metadata has slot 11.
		{.name = "Ensemble",
		 .count = 11,
		 .poly_param = 13,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {},
		 .integers = {5},
		 .gains = {13.f / 12, .5, .5, .5, .5, .5, .5, .5, .5, .5, .5},
		 .extra = {{11, (45.2326448f + 60) / 130}, {12, 1}}},
		{.name = "Vocoder",
		 .count = 12,
		 .poly_param = 11,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {},
		 .integers = {5, 8},
		 .gains = {.5, .5, .5, .5, .5, .5, .6, .5, .5, .5, .5, .5},
		 .extra = {},
		 .outputs = 2,
		 .poly_default = false,
		 .input_routes = {{2, 2}, {3, 3}},
		 .booleans = {},
		 .overridden = {},
		 .parameter_ids = {0, 1, 2, 3, -1, 9, 4, 5, 10, 6, 7, 8},
		 .cv_ids = {4, 5, 6, 7, -1, -1, 8, 9, -1, 10, 11, 12}},
		// Rack captures modScales before Extend changes the parameter range.
		// Frequency CV gain therefore remains the original narrow-band span.
		{.name = "Resonator",
		 .count = 12,
		 .poly_param = 18,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {12, 13, 14},
		 .integers = {9},
		 .gains = {27.8631f / 120, .5, .5, 27.86315f / 120, .5, .5, 27.86313f / 120, .5, .5, .5, .5, .5},
		 .extra = {{15, 0}, {16, 0}, {17, 0}},
		 .outputs = 2,
		 .poly_default = false,
		 .input_routes = {},
		 .booleans = {},
		 .overridden = {},
		 .parameter_ids = {},
		 .cv_ids = {2, 3, 4, 5, 6, 7, 8, 9, 10, -1, 11, 12}},
		{.name = "SpringReverb",
		 .count = 8,
		 .poly_param = 8,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {6},
		 .integers = {},
		 .gains = {},
		 .extra = {},
		 .outputs = 2,
		 .poly_default = false,
		 .input_routes = {{9, 9}},
		 .booleans = {},
		 .overridden = {6},
		 .parameter_ids = {},
		 .cv_ids = {2, 3, 4, 5, 6, 7, 10, 8}},
		{.name = "Nimbus",
		 .count = 12,
		 .poly_param = 12,
		 .clock_param = -1,
		 .clock_input = -1,
		 .switches = {8},
		 .integers = {0, 1},
		 .gains = {},
		 .extra = {},
		 .outputs = 2,
		 .poly_default = false,
		 .input_routes = {{9, 11}, {10, 12}},
		 .booleans = {},
		 .overridden = {8},
		 .parameter_ids = {},
		 .cv_ids = {-1, -1, 2, 3, 4, 5, 6, 7, 13, 8, 9, 10}}};
	for (auto const &s : specs)
		if (model == "SurgeXTFX" + std::string(s.name))
			return &s;
	return nullptr;
}
struct Ref {
	unsigned module, parameter;
	float lo = 0, hi = 1, source_limit = 1;
	bool discrete_changed = false;
	std::string error;
};
struct Plan {
	PatchData &p;
	PatchData const source;
	Report report;
	std::set<unsigned> changed;
	std::map<Key, float> params;
	std::map<Key, Ref> refs;
	std::map<Key, std::vector<Jack>> inputs;
	std::map<unsigned, unsigned> output_counts;
	std::set<Key> connected, mapped;
	std::vector<InternalCable> helper_cables;
	std::vector<ModuleInitState> native_states;

	explicit Plan(PatchData &patch)
		: p(patch)
		, source(patch) {
		for (auto const &c : source.int_cables)
			for (auto const &j : c.ins)
				connected.emplace(j.module_id, j.jack_id);
		for (auto const &c : source.mapped_ins)
			for (auto const &j : c.ins)
				connected.emplace(j.module_id, j.jack_id);
		for (auto const &bank : source.knob_sets)
			for (auto const &m : bank.set)
				mapped.emplace(m.module_id, m.param_id);
		for (auto const &m : source.midi_maps.set)
			mapped.emplace(m.module_id, m.param_id);
	}
	void set(unsigned mid, unsigned pid, float value) {
		params[{mid, pid}] = bounded(value);
	}
	unsigned helper(char const *slug, unsigned source_id) {
		auto id = p.module_slugs.size();
		require(id < 64, "Surge conversion exceeds the 64-module patch limit");
		p.module_slugs.emplace_back(slug);
		const auto alias = "Surge " + std::to_string(source_id) + " CV depths";
		p.module_aliases.push_back({uint16_t(id), alias.c_str()});
		++report.helper_modules;
		return unsigned(id);
	}
	void connect(unsigned from, unsigned output, unsigned to, unsigned input) {
		helper_cables.push_back({{uint16_t(from), uint16_t(output)}, {{uint16_t(to), uint16_t(input)}}, 0xfc80});
	}
	void modulation(unsigned mid,
					unsigned start,
					unsigned input,
					unsigned count,
					bool poly,
					std::map<unsigned, float> const &raw,
					int native_base = -1,
					std::vector<float> const &gains = {},
					std::vector<unsigned> const &ignored = {},
					std::vector<int> const &cv_ids = {},
					bool force_matrix = false,
					std::vector<unsigned> const &required_targets = {},
					std::string const &matrix_destination = {}) {
		if (native_base < 0)
			native_base = int(input);
		auto gain = [&](unsigned target) {
			return gains.empty() ? .5f : gains.at(target);
		};
		auto audible = [&](unsigned target) {
			return std::find(ignored.begin(), ignored.end(), target) == ignored.end();
		};
		auto destination = [&](unsigned target) {
			return cv_ids.empty() ? native_base + int(target) : cv_ids.at(target);
		};
		auto depth = [&](unsigned target, unsigned src) {
			auto i = raw.find(start + 4 * target + src);
			return i == raw.end() ? 0.f : 2 * bounded(i->second) - 1;
		};
		std::vector<Key> active;
		// Selector conversion can require an output even with no modulation.
		// Requesting a matrix controls its type, not whether all jacks are wired.
		std::set<unsigned> targets(required_targets.begin(), required_targets.end());
		bool compact = !force_matrix, needed = !required_targets.empty();
		for (unsigned s = 0; s < 4; ++s) {
			inputs[{mid, input + s}] = {};
		}
		for (unsigned t = 0; t < count; ++t)
			for (unsigned s = 0; s < 4; ++s) {
				if (depth(t, s) != 0 || mapped.contains({mid, start + 4 * t + s})) {
					active.emplace_back(t, s);
					const bool first_target = targets.insert(t).second;
					// Firmware sums stacked cables per lane. That is exact for
					// mono destinations, but loses mono-to-poly broadcasting when
					// mixed-width sources are combined, so retain a mixer there.
					compact &= (first_target || !poly) && connected.contains({mid, input + s}) && gain(t) <= 1;
				}
			}
		needed |= !active.empty();
		compact &= !active.empty() && active.size() <= 4;
		if (compact) {
			for (unsigned first = 0; first < active.size(); first += 2) {
				auto h = helper("4msCompany:Atvert2", mid);
				set(h, 0, .5);
				set(h, 1, .5);
				for (unsigned ch = 0; ch < 2 && first + ch < active.size(); ++ch) {
					auto [t, s] = active[first + ch];
					set(h, ch, .5f + depth(t, s) * gain(t) / 2);
					refs[{mid, start + 4 * t + s}] = {h, ch, .5f - gain(t) / 2, .5f + gain(t) / 2};
					inputs[{mid, input + s}].push_back({uint16_t(h), uint16_t(ch)});
					if (audible(t) && destination(t) >= 0)
						connect(h, ch, mid, destination(t));
				}
			}
		} else if (needed) {
			for (unsigned first = 0; first < count; first += 8) {
				bool used = false;
				for (unsigned t = first; t < std::min(first + 8, count); ++t)
					used |= targets.contains(t);
				if (!used)
					continue;
				const auto matrix_slug = "SurgeXTNative:ModMatrix" + matrix_destination;
				auto h = helper(matrix_slug.c_str(), mid);
				if (!matrix_destination.empty()) {
					const auto alias = matrix_destination + " " + std::to_string(mid) + " CV depths";
					p.module_aliases.back().alias_name = alias.c_str();
					report.notes.push_back(
						"Named CV depth menus require SurgeXTNative 0.1.42 and firmware 2.4 or later.");
				}
				for (unsigned i = 0; i < 40; ++i)
					set(h, i, .5);
				set(h, 40, poly ? 1 : 0);
				for (unsigned s = 0; s < 4; ++s) {
					const bool assigned = std::any_of(active.begin(), active.end(), [&](auto route) {
						return route.second == s && route.first >= first && route.first < std::min(first + 8, count);
					});
					if (assigned)
						inputs[{mid, input + s}].push_back({uint16_t(h), uint16_t(s)});
				}
				for (unsigned t = first; t < std::min(first + 8, count); ++t) {
					for (unsigned s = 0; s < 4; ++s) {
						auto pid = 8 + 4 * (t - first) + s;
						set(h, pid, .5f + depth(t, s) * gain(t) / 4);
						refs[{mid, start + 4 * t + s}] = {h, pid, .5f - gain(t) / 4, .5f + gain(t) / 4};
					}
					if (targets.contains(t) && audible(t) && destination(t) >= 0)
						connect(h, t - first, mid, destination(t));
				}
			}
		}
		if (needed)
			report.notes.push_back("Module " + std::to_string(mid) +
								   ": CV helpers add a scheduling stage; hardware timing remains to be measured.");
	}
	void module(unsigned mid, std::string const &slug) {
		auto model = slug.substr(std::string("SurgeXTRack:").size());
		auto *effect = effect_spec(model);
		const bool oscillator = model == "SurgeXTOSCAlias" || model == "SurgeXTOSCClassic" ||
								model == "SurgeXTOSCModern" || model == "SurgeXTOSCSine" ||
								model == "SurgeXTOSCSHNoise" || model == "SurgeXTOSCFM2" || model == "SurgeXTOSCFM3" ||
								model == "SurgeXTOSCTwist" || model == "SurgeXTOSCString" ||
								model == "SurgeXTOSCWavetable" || model == "SurgeXTOSCWindow";
		require(model == "SurgeXTDelay" || model == "SurgeXTDelayLineByFreq" ||
					model == "SurgeXTDelayLineByFreqExpanded" || model == "SurgeXTLFO" || model == "SurgeXTModMatrix" ||
					model == "SurgeXTMixer" || model == "SurgeXTMixerSlider" || model == "SurgeXTDigitalRingMod" ||
					model == "SurgeXTVCF" || model == "SurgeXTWaveshaper" || model == "SurgeXTEGxVCA" ||
					model == "SurgeXTQuadAD" || model == "SurgeXTQuadLFO" || model == "SurgeXTUnisonHelper" ||
					model == "SurgeXTUnisonHelperCVExpander" || oscillator || effect,
				"Surge module " + model + " has no audited native export yet. Nothing was sent or saved.");
		changed.insert(mid);
		++report.converted_modules;
		std::map<unsigned, float> raw;
		for (auto const &v : source.static_knobs)
			if (v.module_id == mid) {
				require(raw.emplace(v.param_id, v.value).second, "Duplicate Surge parameter");
			}
		std::set<unsigned> consumed;
		auto value = [&](unsigned id, float fallback = 0.f, float max = 1.f) {
			consumed.insert(id);
			auto it = raw.find(id);
			return bounded(it == raw.end() ? fallback : it->second, 0, max);
		};
		auto param = [&](unsigned old, unsigned native, float fallback = 0.f) {
			set(mid, native, value(old, fallback));
			refs[{mid, old}] = {mid, native};
		};
		Json state(json_object(), json_decref);
		unsigned state_count = 0;
		for (auto const &s : source.module_states)
			if (s.module_id == mid) {
				require(++state_count == 1, "Duplicate Surge module state");
				json_error_t error{};
				state.reset(json_loadb(s.state_data.data(), s.state_data.size(), JSON_REJECT_DUPLICATES, &error));
				require(bool(state), "Invalid Surge JSON state: " + std::string(error.text));
			}
		allowed(state.get(), {"xtshared", "modulespecific"});
		auto *shared = json_object_get(state.get(), "xtshared");
		if (shared) {
			allowed(shared,
					{"streamingVersion",
					 "buildInfo",
					 "isCoupledToGlobalStyle",
					 "localStyle",
					 "localDisplayRegionColor",
					 "localModulationColor",
					 "localControlValueColor",
					 "localPowerButtonColor"});
			if (auto *v = json_object_get(shared, "streamingVersion"))
				require(json_is_integer(v) && json_integer_value(v) == 1, "Unsupported Surge streaming version");
		}
		auto *specific = json_object_get(state.get(), "modulespecific");
		Json empty(json_object(), json_decref);
		if (!specific)
			specific = empty.get();
		unsigned direct = 0, nout = 2;
		if (model == "SurgeXTUnisonHelper") {
			allowed(specific, {});
			constexpr float defaults[]{.1f, 0, 0, 1, .25f, 0, .5f, 0, 0};
			for (unsigned i = 0; i < 9; ++i)
				param(i, i, defaults[i]);
			modulation(mid, 9, 5, 4, true, raw);
			for (unsigned i = 9; i < 25; ++i)
				consumed.insert(i);
			native_states.push_back({uint16_t(mid), "surge-unison-rack-v1"});
			direct = 5;
			nout = 6;
			report.notes.push_back(
				"Unison imports require SurgeXTNative 0.1.20. Four input notes and 16 total unison voices are supported; larger combinations display Over 16 voices.");
		} else if (model == "SurgeXTUnisonHelperCVExpander") {
			allowed(specific, {});
			direct = 2;
			nout = 8;
		} else if (oscillator) {
			const bool alias = model == "SurgeXTOSCAlias", modern = model == "SurgeXTOSCModern";
			const bool sine = model == "SurgeXTOSCSine", noise = model == "SurgeXTOSCSHNoise";
			const bool fm2 = model == "SurgeXTOSCFM2", fm3 = model == "SurgeXTOSCFM3", fm = fm2 || fm3;
			const bool twist = model == "SurgeXTOSCTwist", string = model == "SurgeXTOSCString";
			const bool window = model == "SurgeXTOSCWindow", table = window || model == "SurgeXTOSCWavetable";
			const unsigned table_extra = window ? 16 : 14;
			const std::vector<unsigned> linear_integers = sine				? std::vector<unsigned>{1, 3} :
														  fm2				? std::vector<unsigned>{2, 4} :
														  (twist || string) ? std::vector<unsigned>{1} :
														  window			? std::vector<unsigned>{3} :
																			  std::vector<unsigned>{};
			if (table)
				allowed(specific,
						{"paramNatural", "halfbandM", "halfbandSteep", "doDCBlock", "displayPolyChannel", "wavetable"});
			else
				allowed(specific, {"paramNatural", "halfbandM", "halfbandSteep", "doDCBlock", "displayPolyChannel"});
			for (unsigned i = 0; i < 8; ++i)
				require(raw.contains(i), "Incomplete Hub oscillator controls");
			if (auto *natural = json_object_get(specific, "paramNatural")) {
				require(json_is_array(natural) && json_array_size(natural) == 7,
						"Invalid oscillator natural parameter metadata");
				for (unsigned i = 0; i < 7; ++i) {
					auto *v = json_array_get(natural, i), *index = json_object_get(v, "index"),
						 *kind = json_object_get(v, "valtype");
					const bool integer =
						(alias && i == 0) || (!fm && !twist && !string && i == 6) ||
						std::find(linear_integers.begin(), linear_integers.end(), i + 1) != linear_integers.end();
					require(json_is_integer(index) && json_integer_value(index) == i && json_is_integer(kind) &&
								json_integer_value(kind) == (integer ? 0 : 2),
							"Oscillator parameter type changed");
				}
			}
			if (auto *channel = json_object_get(specific, "displayPolyChannel"))
				require(json_is_integer(channel) && json_integer_value(channel) >= 0 &&
							json_integer_value(channel) < 16,
						"Invalid oscillator display channel");
			unsigned order = 6;
			bool steep = true;
			auto *m = json_object_get(specific, "halfbandM"), *s = json_object_get(specific, "halfbandSteep");
			if (m && s) {
				require(json_is_integer(m) && json_integer_value(m) >= 1 && json_integer_value(m) <= 6 &&
							json_is_boolean(s),
						"Invalid oscillator output filter");
				order = unsigned(json_integer_value(m));
				steep = json_is_true(s);
			}
			native_states.push_back(
				{uint16_t(mid),
				 table ? table_state(specific, order, steep, json_bool(specific, "doDCBlock", true)) :
						 std::string("surge-osc-rack-v1 ") + char('0' + order) + " " + char('0' + steep) + " " +
							 char('0' + json_bool(specific, "doDCBlock", true))});
			param(0, 0, .5);
			for (unsigned i = 2; i < 7; ++i)
				param(i, i);
			// The helper translates the non-linear waveform enum AFTER summing
			// its base value and CV. It also leaves pitch and octave independent.
			auto h = unsigned(p.module_slugs.size());
			std::vector<float> gains(8, .5);
			gains[0] = 1;
			for (auto id : linear_integers)
				gains[id] /= .99f;
			auto required_targets = linear_integers;
			if (alias)
				required_targets.push_back(1);
			if (!fm && !twist && !string)
				required_targets.push_back(7);
			modulation(mid,
					   8,
					   2,
					   8,
					   true,
					   raw,
					   -1,
					   gains,
					   {},
					   {(alias || string) ? 12 : 11, 2, 3, 4, 5, 6, 7, 8},
					   true,
					   required_targets,
					   model.substr(10));
			if (alias) {
				set(h, 1, .5f + value(1) / 4);
				set(h, 42, .5);
				set(mid, 1, 0);
				refs[{mid, 1}] = {h, 1, .5, .75, 1, true};
			} else
				param(1, 1);
			for (unsigned id : linear_integers) {
				// Keep the endpoint correction outside native knob clamping:
				// the linear helper applies it to the complete base + CV sum.
				set(mid, id, 0);
				set(h, id, .5f + (value(id) - .005f) / (.99f * 4));
				refs[{mid, id}] = {h, id, .5f - .005f / (.99f * 4), .5f + .995f / (.99f * 4), 1, true};
			}
			if (fm || twist || string)
				param(7, 7);
			else {
				set(h, 7, .5f + value(7) / 4);
				set(h, 48, 1);
				set(mid, 7, 0);
				refs[{mid, 7}] = {h, 7, .5, .75, 1, true};
			}
			for (unsigned i = 8; i < 40; ++i)
				value(i, .5);
			param(40, alias ? 31 : twist ? 14 : string ? 19 : table ? table_extra : 10, .5);
			for (unsigned i = 41; i < 45; ++i) {
				if (modern && i < 44)
					param(i, i - 26);
				else if (fm2 && i < 43)
					param(i, i - 30);
				else if (fm3)
					param(i, i < 43 ? i - 26 : i - 32);
				else if (twist && i == 41) {
					set(mid, 10, .5f + value(i) / 2);
					refs[{mid, i}] = {mid, 10, .5, 1, 1, true};
				} else if (string && i == 41)
					param(i, 10);
				else if (window && i < 43)
					param(i, i - 27);
				else if ((sine || noise) && i < 43) {
					set(mid, i - 26, .5f + value(i) / 2);
					refs[{mid, i}] = {mid, i - 26, .5, 1, 1, true};
				} else if (sine && i == 43)
					param(i, 17);
				else
					value(i);
			}
			if (sine)
				set(mid, 18, 1); // Rack permanently extends feedback range.
			param(45, alias ? 30 : twist ? 13 : string ? 18 : table ? 13 : 14);
			if (fm || twist || string) {
				value(46);
				value(47);
			} else {
				param(46, alias ? 27 : 11);
				param(47, alias ? 28 : 12);
			}
			param(48, alias ? 29 : twist ? 12 : string ? 17 : table ? table_extra + 1 : 13, .5);
			param(49, 8);
			if (table)
				set(mid, 10, 1); // Rack uses smooth table morphing.
			if (twist)
				set(mid, 11, 0); // Rack always uses the ordinary mono aux mix.
			if (string) {		 // These controls are fixed by the original Rack wrapper.
				set(mid, 11, 1);
				set(mid, 12, 1); // Sinc interpolation, compensated stiffness.
				for (unsigned i = 13; i < 17; ++i)
					set(mid, i, 0); // Positive decay, ordinary relative detune.
			}
			if (alias) {
				for (unsigned i = 50; i < 66; ++i)
					param(i, i - 40, .5);
				param(66, 9, 1);
				set(mid, 26, 0);
				inputs[{mid, 6}] = {{uint16_t(mid), 11}};
			} else {
				param(50, 9, 1);
				inputs[{mid, 6}] = string ? std::vector<Jack>{{uint16_t(mid), 11}} : std::vector<Jack>{};
			}
			direct = 2;
			report.notes.push_back(
				alias ?
					"Alias imports require SurgeXTNative 0.1.8 or later. Wave selection and depths are editable in the CV helper; Rack memory-reading waves use Audio/Noise fallbacks." :
				table ?
					"Wavetable/Window imports require SurgeXTNative 0.1.14 or later. Table samples and output settings travel inside the patch; selectors and depths are editable in the CV helper." :
				string ?
					"String imports require SurgeXTNative 0.1.13 or later. Exciter selection and depths are editable in the CV helper; the audio input uses the first channel as in Rack." :
				twist ?
					"Twist imports require SurgeXTNative 0.1.12 or later. Engine selection and depths are editable in the CV helper; the saved LPG enable is preserved." :
				fm ?
					"FM2/FM3 imports require SurgeXTNative 0.1.11 or later. Ratio selectors and depths are editable in the CV helper." :
				(sine || noise) ?
					"Sine/SHNoise imports require SurgeXTNative 0.1.10 or later. Selector controls and depths are editable in the CV helper." :
					"Classic/Modern imports require SurgeXTNative 0.1.9 or later. Unison selection and depths are editable in the CV helper.");
		} else if (effect) {
			auto const &e = *effect;
			allowed(specific,
					{"loadedPreset", "presetName", "presetIsDirty", "polyphonicMode", "clockStyle", "paramNatural"});
			// The Hub writes every configured parameter. Do not guess a missing
			// value from native defaults: Rack initializes from its preset bank.
			for (unsigned i = 0; i < e.count; ++i) {
				require(raw.contains(i), "Incomplete Hub FX controls for " + model);
				const int native = e.parameter_ids.empty() ? int(i) : e.parameter_ids.at(i);
				if (native < 0) {
					value(i);
					continue;
				}
				param(i, native);
				// Some Rack engine slots are overwritten each block by an
				// exposed switch/gate. Mapping the hidden slot to that switch
				// would change an inert Rack mapping into an audible control.
				if (std::find(e.overridden.begin(), e.overridden.end(), i) != e.overridden.end())
					refs.at({mid, i}).error = "Rack overwrites this engine slot; map its exposed switch instead";
				if (std::find(e.integers.begin(), e.integers.end(), i) != e.integers.end()) {
					set(mid, native, std::clamp((value(i) - .005f) / .99f, 0.f, 1.f));
					refs[{mid, i}] = {mid, unsigned(native), -.005f / .99f, .995f / .99f, 1, true};
					if (model == "SurgeXTFXVocoder" && i == 5) {
						// Source rounds 4..20, then floors to a multiple of 4.
						// Shift native's five-position thresholds by 1.5/16.
						const auto bands = std::clamp(rounded(4 + 16 * (value(i) - .005f) / .99f), 4u, 20u) / 4;
						set(mid, native, (bands - 1) / 4.f);
						refs[{mid, i}].lo -= 1.5f / 16;
						refs[{mid, i}].hi -= 1.5f / 16;
					}
				}
			}
			for (unsigned i = e.count; i < 12; ++i)
				value(i);
			for (unsigned i = 0; i < e.switches.size(); ++i) {
				require(raw.contains(60 + i), "Incomplete Hub FX switches for " + model);
				param(60 + i, e.switches[i]);
			}
			if (model == "SurgeXTFXSpringReverb" || model == "SurgeXTFXNimbus") {
				const bool spring = model == "SurgeXTFXSpringReverb";
				native_states.push_back({uint16_t(mid), spring ? "surge-spring-rack-v1" : "surge-nimbus-rack-v1"});
				if (!spring)
					value(61); // Randomize Mode/Quality is inert during playback.
				report.notes.push_back(
					"Spring/Nimbus imports require SurgeXTNative 0.1.15 or later. Signed Knock/Freeze depths use the dedicated CV jack after the button/gate calculation.");
			}
			// This descriptive array is not restored by Rack. Validate its
			// type signature when present to detect a changed source layout.
			if (auto *natural = json_object_get(specific, "paramNatural")) {
				require(json_is_array(natural) && json_array_size(natural) == 12,
						"Invalid FX natural parameter metadata");
				for (unsigned i = 0; i < e.count; ++i) {
					if (!e.parameter_ids.empty() && e.parameter_ids[i] < 0)
						continue;
					auto *v = json_array_get(natural, i), *index = json_object_get(v, "index"),
						 *kind = json_object_get(v, "valtype");
					bool discrete = std::find(e.integers.begin(), e.integers.end(), i) != e.integers.end();
					bool boolean = std::find(e.booleans.begin(), e.booleans.end(), i) != e.booleans.end();
					require(json_is_integer(index) && json_integer_value(index) == i && json_is_integer(kind) &&
								json_integer_value(kind) == (discrete ? 0 :
															 boolean  ? 1 :
																		2),
							"FX parameter type changed for " + model);
				}
			}
			const bool poly = json_bool(specific, "polyphonicMode", e.poly_default);
			set(mid, e.poly_param, poly ? 1 : 0);
			if (e.clock_param >= 0)
				set(mid, e.clock_param, e.clock_input >= 0 ? json_number(specific, "clockStyle", 0) : 0);
			if (e.clock_input >= 0) {
				native_states.push_back({uint16_t(mid), "surge-fx-clock-rack-v1"});
				report.notes.push_back(
					"Clocked FX imports require SurgeXTNative 0.1.28 or later for original Rack clock connection and format history.");
			}
			if (model == "SurgeXTFXExciter" || model == "SurgeXTFXEnsemble") {
				native_states.push_back({uint16_t(mid), "surge-fx-history-rack-v1"});
				report.notes.push_back(
					"Exciter/Ensemble imports require SurgeXTNative 0.1.34 or later for original Rack Sum/Poly initialization and modulation history.");
			}
			if (model == "SurgeXTFXChow") {
				native_states.push_back({uint16_t(mid), "surge-fx-history-rack-v1"});
				report.notes.push_back(
					"Chow imports require SurgeXTNative 0.1.35 or later for Rack Sum/Poly initialization and smoother history. The native Mix-output correction remains enabled.");
			}
			if (model == "SurgeXTFXTreeMonster") {
				native_states.push_back({uint16_t(mid), "surge-fx-history-rack-v1"});
				report.notes.push_back(
					"Tree Monster imports require SurgeXTNative 0.1.36 or later for Rack Sum/Poly engine and pitch/envelope detector history.");
			}
			if (model == "SurgeXTFXReverb2") {
				native_states.push_back({uint16_t(mid), "surge-fx-history-rack-v1"});
				report.notes.push_back(
					"Reverb 2 imports require SurgeXTNative 0.1.37 or later for retained Rack Sum/Poly tails and full modulated Room Size storage.");
			}
			if (model == "SurgeXTFXReverb") {
				native_states.push_back({uint16_t(mid), "surge-fx-history-rack-v1"});
				report.notes.push_back(
					"Reverb imports require SurgeXTNative 0.1.45 or later for Rack Sum/Poly initialization, retained tails and modulation after base controls.");
			}
			if (model == "SurgeXTFXCombulator") {
				native_states.push_back({uint16_t(mid), "surge-fx-history-rack-v1"});
				report.notes.push_back(
					"Combulator imports require SurgeXTNative 0.1.38 or later for Rack Sum/Poly engine history and the shared noise generator. Rack does not save the original random seed.");
			}
			if (model == "SurgeXTFXDistortion") {
				native_states.push_back({uint16_t(mid), "surge-fx-history-rack-v1"});
				report.notes.push_back(
					"Distortion imports require SurgeXTNative 0.1.39 or later for Rack Sum/Poly filter, shaper and smoother history. The native 4x drive-ramp correction remains enabled.");
			}
			if (model == "SurgeXTFXBonsai") {
				native_states.push_back({uint16_t(mid), "surge-fx-history-rack-v1"});
				report.notes.push_back(
					"Bonsai imports require SurgeXTNative 0.1.40 or later for Rack Sum/Poly filter, smoother and noise history, initialization timing and post-base float modulation.");
			}
			if (model == "SurgeXTFXRingMod") {
				native_states.push_back({uint16_t(mid), "surge-ringmod-rack-v1"});
				report.notes.push_back(
					"Ring Modulator imports require SurgeXTNative 0.1.41 or later for Rack engine history and a shared summed carrier. Poly external-carrier upsampling and auto-selection are corrected; pinned Rack leaves that buffer stale.");
			}
			if (model == "SurgeXTFXVocoder") {
				native_states.push_back({uint16_t(mid), "surge-vocoder-rack-v1"});
				report.notes.push_back(
					"Vocoder imports require SurgeXTNative 0.1.43 or later for Rack engine/filter history, main-audio voice counts and shared summed modulator routing. Rack's five-times-higher Poly modulator level is preserved.");
			}
			for (auto [id, v] : e.extra)
				set(mid, id, v);
			for (unsigned i = 2; i <= 4; ++i)
				inputs[{mid, i}] = {};
			if (e.clock_input >= 0)
				inputs[{mid, 4}] = {{uint16_t(mid), uint16_t(e.clock_input)}};
			for (auto [old, native] : e.input_routes)
				inputs[{mid, old}] = {{uint16_t(mid), uint16_t(native)}};
			auto ignored = e.integers;
			ignored.insert(ignored.end(), e.booleans.begin(), e.booleans.end());
			modulation(mid, 12, 5, e.count, poly, raw, 2, e.gains, ignored, e.cv_ids);
			for (unsigned i = 12; i < 60; ++i)
				value(i, .5);
			direct = 2;
			nout = e.outputs;
		} else if (model == "SurgeXTVCF" || model == "SurgeXTWaveshaper") {
			const bool vcf = model == "SurgeXTVCF";
			for (unsigned i = 0; i < 5; ++i) {
				require(raw.contains(i), "Incomplete Hub filter controls");
				param(i, i);
			}
			require(raw.contains(25), "Missing Hub filter model");
			param(25, 5);
			if (vcf) {
				require(raw.contains(26), "Missing Hub filter subtype");
				set(mid, 6, std::min(rounded(value(26) * 12), 11u) / 11.f);
				refs[{mid, 26}] = {mid, 6, 0, 12.f / 11, 1, true};
				allowed(specific, {"displayPolyChannel"});
				native_states.push_back({uint16_t(mid), "surge-vcf-rack-v1"});
				report.notes.push_back(
					"VCF imports require SurgeXTNative 0.1.7 or later; invalid subtype requests reset to zero. Rack's invalid global subtype 12 endpoint saturates to 11.");
			} else {
				// Rack's inclusive maximum is the enum count (43), not the
				// highest valid model (42). Preserve all valid model thresholds.
				set(mid, 5, std::min(value(25) * 43.f / 42, 1.f));
				refs[{mid, 25}] = {mid, 5, 0, 43.f / 42, 1, true};
				for (unsigned i = 26; i < 28; ++i) {
					require(raw.contains(i), "Missing Hub filter enable");
					param(i, i - 20);
				}
				allowed(specific, {"displayPolyChannel", "doDCBlock"});
				// Without an explicitly saved flag, Rack disables the blocker
				// on its first processing block (wasDoDCBlockSetByJSON).
				set(mid, 8, json_bool(specific, "doDCBlock", false) ? 1 : 0);
				report.notes.push_back("Waveshaper's invalid model 43 endpoint saturates to the last valid model, 42.");
			}
			if (auto *channel = json_object_get(specific, "displayPolyChannel"))
				require(json_is_integer(channel) && json_integer_value(channel) >= 0 &&
							json_integer_value(channel) < 16,
						"Invalid filter display channel");
			modulation(mid,
					   5,
					   2,
					   5,
					   true,
					   raw,
					   2,
					   {vcf ? 1.f : .5f, .5, .5, .5, .5},
					   {},
					   {},
					   false,
					   {},
					   vcf ? "VCF" : "Waveshaper");
			for (unsigned i = 5; i < 25; ++i)
				value(i, .5);
			direct = 2;
		} else if (model == "SurgeXTDelay") {
			constexpr std::array<float, 10> defaults{2.f / 3, 2.f / 3, .5, .5, 0, 0, 1, .5, 0, 1};
			for (unsigned i = 0; i < 10; ++i)
				param(i, i, defaults[i]);
			set(mid, 10, rounded(value(50, 1) * 3) / 3.f);
			refs[{mid, 50}] = {mid, 10, 0, 1, 1, true};
			allowed(specific, {"clockStyle"});
			set(mid, 11, json_number(specific, "clockStyle", 0));
			native_states.push_back({uint16_t(mid), "surge-delay-rack-v1"});
			report.notes.push_back(
				"Delay imports require SurgeXTNative 0.1.23 or later. Saved compatibility state preserves Rack's initial clock edge, tempo history across disconnects/format changes and BPM-voltage calculation.");
			modulation(mid, 10, 3, 10, false, raw);
			for (unsigned i = 10; i < 50; ++i)
				consumed.insert(i);
			direct = 3;
		} else if (model == "SurgeXTDelayLineByFreq") {
			param(0, 0, .5);
			param(1, 1);
			require(std::abs(params[{mid, 1}] * 20 - std::round(params[{mid, 1}] * 20)) < 1e-4,
					"Fractional sample correction cannot be preserved");
			allowed(specific, {});
			direct = 3;
		} else if (model == "SurgeXTDelayLineByFreqExpanded") {
			constexpr std::array<float, 11> defaults{.5, .5, .5, .98, 1, 0, 1, 0, 0, .05, 0};
			for (unsigned i = 0; i < 11; ++i)
				param(i, i, defaults[i]);
			require(std::abs(params[{mid, 9}] * 20 - std::round(params[{mid, 9}] * 20)) < 1e-4,
					"Fractional sample correction cannot be preserved");
			// Rack casts the clamp selector to an integer; native menus round.
			set(mid, 11, std::min(2u, unsigned(value(39, .5f) * 2)) / 2.f);
			refs[{mid, 39}] = {mid, 11, -.25f, .75f, 1, true};
			allowed(specific, {});
			direct = 6;
			native_states.push_back({uint16_t(mid), "surge-tuned-plus-rack-v1"});
			modulation(mid, 11, 6, 7, true, raw, -1, {1, .5, .5, .5, .5, .5, .5}, {}, {13, 6, 7, 8, 9, 10, 11});
			for (unsigned i = 11; i < 39; ++i)
				consumed.insert(i);
			report.notes.push_back(
				"Tuned Delay + imports require SurgeXTNative 0.1.16 or later. Pitch modulation uses its separate jack and does not create additional voices.");
		} else if (model == "SurgeXTModMatrix") {
			for (unsigned i = 0; i < 40; ++i)
				param(i, i, .5);
			set(mid, 40, 0);
			allowed(specific, {});
			direct = 4;
			nout = 8;
		} else if (model == "SurgeXTMixer" || model == "SurgeXTMixerSlider") {
			constexpr std::array<float, 20> defaults{1, 0, 0, 0, 0, 0, .5, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
			for (unsigned i = 0; i < 20; ++i)
				param(i, i, defaults[i]);
			allowed(specific, {"vuChannel"});
			auto *meter = json_object_get(specific, "vuChannel");
			require(!meter ||
						(json_is_integer(meter) && json_integer_value(meter) >= 0 && json_integer_value(meter) < 16),
					"Invalid Mixer meter channel");
			const auto channel = meter ? unsigned(json_integer_value(meter)) : 0;
			set(mid, 20, std::min(channel, 3u) / 3.f);
			if (channel > 3)
				report.notes.push_back("Mixer meter channel limited to the four native voices");
			// Rack's readModuleSpecificJson disables automatic first-connect
			// unmute. Preserve saved mutes through the equivalent native state.
			native_states.push_back({uint16_t(mid), "surge-mixer-v1"});
			modulation(mid, 20, 6, 8, true, raw);
			for (unsigned i = 20; i < 52; ++i)
				consumed.insert(i);
			direct = 6;
		} else if (model == "SurgeXTQuadLFO") {
			for (unsigned i = 0; i < 16; ++i)
				param(i, i, i < 4 ? 5.f / 13 : i < 8 ? .5f : i >= 12 ? 1.f : 0.f);
			param(48, 16);
			allowed(specific, {"clockStyle", "forcePolyphony"});
			set(mid, 18, json_number(specific, "clockStyle", 0));
			auto *forced = json_object_get(specific, "forcePolyphony");
			require(!forced || (json_is_integer(forced) && json_integer_value(forced) >= -1 &&
								json_integer_value(forced) <= 16),
					"Invalid Quad LFO forcePolyphony");
			auto voices = forced ? json_integer_value(forced) : -1;
			set(mid, 17, std::clamp(int(voices), 0, 4) / 4.f);
			if (voices > 4)
				report.notes.push_back("Quad LFO forced polyphony limited to four native voices");
			native_states.push_back({uint16_t(mid), "surge-quad-lfo-rack-v1"});
			modulation(mid, 16, 4, 8, true, raw);
			for (unsigned i = 16; i < 48; ++i)
				consumed.insert(i);
			direct = 4;
			nout = 4;
			report.notes.push_back(
				"Quad LFO imports require SurgeXTNative 0.1.19 or later. Saved compatibility state preserves Rack's clock, mode/phase history, freeze/reverse routing and extended modulation.");
		} else if (model == "SurgeXTQuadAD") {
			for (unsigned i = 0; i < 32; ++i)
				param(i, i, i < 8 ? 3.f / 11.32192809489f : (i >= 12 && i < 20 || i >= 24 && i < 28) ? .5f : 0.f);
			allowed(specific, {"attackFromZero"});
			set(mid, 32, json_bool(specific, "attackFromZero", false) ? 1 : 0);
			native_states.push_back({uint16_t(mid), "surge-quad-ad-rack-v1"});
			modulation(mid, 32, 4, 8, true, raw);
			for (unsigned i = 32; i < 64; ++i)
				consumed.insert(i);
			direct = 4;
			nout = 4;
			report.notes.push_back(
				"Quad AD imports require SurgeXTNative 0.1.18 or later. Saved compatibility state preserves Rack's trigger links, ordered sum accumulation, block timing and modulation beyond knob limits.");
		} else if (model == "SurgeXTEGxVCA") {
			constexpr std::array<float, 12> defaults{.5, .5, 0, .1, .1, .5, .1, 0, .5, .5, .5, 0};
			for (unsigned i = 0; i < 12; ++i)
				param(i, i, defaults[i]);
			for (unsigned i = 40; i < 44; ++i)
				param(i, i - 28, i == 40 ? 1 : 0);
			allowed(specific, {"clockStyle"});
			set(mid, 16, json_number(specific, "clockStyle", 0));
			native_states.push_back({uint16_t(mid), "surge-eg-vca-rack-v1"});
			modulation(mid, 12, 4, 7, true, raw);
			for (unsigned i = 12; i < 40; ++i)
				consumed.insert(i);
			direct = 4;
			nout = 4;
			report.notes.push_back(
				"EG x VCA imports require SurgeXTNative 0.1.17 or later. Saved compatibility state preserves Rack's tempo calculation, event timing and modulation beyond knob limits.");
		} else if (model == "SurgeXTDigitalRingMod") {
			for (unsigned i = 0; i < 3; ++i)
				param(i, i);
			allowed(specific, {});
			direct = 8;
			nout = 4;
		} else {
			constexpr std::array<float, 10> defaults{7.f / 16, 0, .5, 1, 0, 0, 8.f / 13, 8.f / 13, 1, 1};
			for (unsigned i = 0; i < 10; ++i)
				param(i, i, defaults[i]);
			const auto encoded = value(10, .005f / .775f) * .775f;
			const unsigned shape = std::min(7u, rounded((encoded - .005f) * 9 / .99f));
			set(mid, 10, shape / 7.f);
			refs[{mid, 10}] = {mid, 10, -.005f * 9 / (.99f * 7), 1, 1, true};
			set(mid, 11, value(11) > .5f ? 1 : 0);
			refs[{mid, 11}] = {mid, 11};
			value(52);
			constexpr unsigned kinds[]{3, 3, 1, 3, 1, 2, 3, 2};
			auto deform = value(53) * 4;
			set(mid, 12, std::min(rounded(deform), kinds[shape] - 1) / 2.f);
			refs[{mid, 53}] = {mid, 12, 0, 2, 1, true};
			set(mid, 13, rounded(value(54, 1.f / 3) * 3) / 3.f);
			refs[{mid, 54}] = {mid, 13, 0, 1, 1, true};
			set(mid, 14, value(55) > .5f ? 1 : 0);
			refs[{mid, 55}] = {mid, 14};
			for (unsigned id : {56u, 57u}) {
				auto role = rounded(value(id, 0, 4.f / 3) * 3);
				set(mid, id - 41, role / 4.f);
				refs[{mid, id}] = {mid, id - 41, 0, .75f, 1, true};
			}
			for (unsigned i = 0; i < 16; ++i) {
				param(58 + i, 17 + i, .5f + .5f * i / 15);
				param(74 + i, 33 + i);
			}
			param(90, 49);
			param(91, 50, 1);
			param(92, 51, 1);
			auto voices = rounded(1 + 15 * value(93));
			require(voices <= 4, "LFO voices exceed four-channel native cable capacity");
			set(mid, 52, (voices - 1) / 3.f);
			refs[{mid, 93}] = {mid, 52, 0, 5, .2f, true};
			param(94, 53);
			param(95, 54);
			value(96, 1);
			allowed(specific, {"clockStyle", "retriggerFromZero", "onepoleFactor"});
			set(mid, 55, json_number(specific, "clockStyle", 0));
			auto *retrigger = json_object_get(specific, "retriggerFromZero");
			require(!retrigger || json_is_boolean(retrigger), "retriggerFromZero must be boolean");
			set(mid, 56, json_is_true(retrigger) ? 1 : 0);
			set(mid, 57, json_number(specific, "onepoleFactor", .75));
			native_states.push_back({uint16_t(mid), "surge-lfo-rack-v1"});
			report.notes.push_back(
				"LFO imports require SurgeXTNative 0.1.22 or later. Saved compatibility state preserves Rack's shape/deform transitions, trigger roles, gate and clock history, and mono event outputs.");
			modulation(mid, 12, 4, 10, true, raw);
			for (unsigned i = 12; i < 52; ++i)
				consumed.insert(i);
			direct = 4;
			nout = 6;
		}
		for (auto const &[id, v] : raw)
			require(consumed.contains(id), "Unknown Surge parameter ID " + std::to_string(id));
		for (unsigned i = 0; i < direct; ++i)
			inputs[{mid, i}] = {{uint16_t(mid), uint16_t(i)}};
		output_counts[mid] = nout;
		p.module_slugs[mid] =
			("SurgeXTNative:" + (effect ? std::string(effect->name) : model.substr(oscillator ? 10 : 7))).c_str();
	}
	Jack output(Jack jack) {
		require(jack.module_id < source.module_slugs.size(), "Cable references missing module");
		if (changed.contains(jack.module_id))
			require(jack.jack_id < output_counts.at(jack.module_id), "Unknown Surge output jack");
		return jack;
	}
	std::vector<Jack> input(Jack jack) {
		require(jack.module_id < source.module_slugs.size(), "Cable references missing module");
		if (!changed.contains(jack.module_id))
			return {jack};
		auto it = inputs.find({jack.module_id, jack.jack_id});
		require(it != inputs.end(), "Unknown Surge input jack");
		return it->second;
	}
	void remap(MappedKnob &m) {
		if (!changed.contains(m.module_id))
			return;
		auto it = refs.find({m.module_id, m.param_id});
		require(it != refs.end(), "Surge mapped parameter has no native control");
		auto const &r = it->second;
		require(r.error.empty(), r.error);
		bounded(m.min, 0, r.source_limit);
		bounded(m.max, 0, r.source_limit);
		require(m.curve_type <= MappedKnob::Cycle, "Unknown mapping curve");
		// Hub's physical pots apply latching in Rack, but firmware ignores it
		// on pots. Existing button/MIDI curves can pass when positions match.
		require(!m.is_panel_knob() || m.curve_type == MappedKnob::Normal,
				"Toggle/Cycle pot mapping differs on MetaModule; use an explicit button mapping");
		require(m.curve_type != MappedKnob::Cycle || !r.discrete_changed,
				"Cycle positions differ after native conversion");
		m.module_id = r.module;
		m.param_id = r.parameter;
		m.min = r.lo + m.min * (r.hi - r.lo);
		m.max = r.lo + m.max * (r.hi - r.lo);
	}
	Report run() {
		for (unsigned id = 0; id < source.module_slugs.size(); ++id) {
			std::string slug = source.module_slugs[id].c_str();
			if (slug.starts_with("SurgeXTRack:"))
				module(id, slug);
		}
		if (changed.empty())
			return report;
		require(p.module_slugs.size() <= 64, "Surge conversion exceeds the 64-module patch limit");
		for (auto const &ex : p.expanders)
			if (changed.contains(ex.left_module_id) || changed.contains(ex.right_module_id)) {
				require(ex.left_module_id < source.module_slugs.size() &&
							ex.right_module_id < source.module_slugs.size(),
						"Expander references missing module");
				const std::string_view left = source.module_slugs[ex.left_module_id].c_str(),
									   right = source.module_slugs[ex.right_module_id].c_str();
				require((left == "SurgeXTRack:SurgeXTUnisonHelper" ||
						 left == "SurgeXTRack:SurgeXTUnisonHelperCVExpander") &&
							right == "SurgeXTRack:SurgeXTUnisonHelperCVExpander",
						"Unsupported Surge expander pairing");
			}
		for (auto const &light : p.mapped_lights)
			require(!changed.contains(light.module_id), "Surge light mapping conversion is not audited yet");
		std::set<Key> occupied;
		auto targets = [&](std::vector<Jack> const &old) {
			std::vector<Jack> result;
			for (auto j : old) {
				if (changed.contains(j.module_id))
					require(occupied.emplace(j.module_id, j.jack_id).second,
							"Several sources feed one Surge input; explicit summing is needed");
				auto next = input(j);
				result.insert(result.end(), next.begin(), next.end());
			}
			return result;
		};
		for (auto &c : p.int_cables) {
			c.out = output(c.out);
			c.ins = targets(c.ins);
		}
		std::erase_if(p.int_cables, [](auto const &c) { return c.ins.empty(); });
		for (auto &c : p.mapped_ins)
			c.ins = targets(c.ins);
		std::erase_if(p.mapped_ins, [](auto const &c) { return c.ins.empty(); });
		for (auto &c : p.mapped_outs)
			c.out = output(c.out);
		p.int_cables.insert(p.int_cables.end(), helper_cables.begin(), helper_cables.end());
		for (auto &bank : p.knob_sets)
			for (auto &m : bank.set)
				remap(m);
		for (auto &m : p.midi_maps.set)
			remap(m);
		std::erase_if(p.static_knobs, [&](auto const &v) { return changed.contains(v.module_id); });
		for (auto const &[key, v] : params)
			p.static_knobs.push_back({uint16_t(key.first), uint16_t(key.second), v});
		std::erase_if(p.module_states, [&](auto const &s) { return changed.contains(s.module_id); });
		p.module_states.insert(p.module_states.end(), native_states.begin(), native_states.end());
		// Original positions remain valid for original modules; append helpers
		// on their own row instead of overlapping existing modules.
		if (!p.module_positions.empty()) {
			int row = 0;
			for (auto const &pos : p.module_positions)
				row = std::max(row, int(pos.y) + 1);
			int x = 0;
			for (unsigned id = source.module_slugs.size(); id < p.module_slugs.size(); ++id) {
				require(row < 32767 && x < 32767, "Helper positions exceed patch coordinate range");
				p.module_positions.push_back({uint16_t(id), int16_t(x), int16_t(row)});
				x += std::string_view(p.module_slugs[id].c_str()) == "4msCompany:Atvert2" ? 4 : 12;
			}
		}
		p.clear_load_balance();
		return report;
	}
};
// Convert a 16-lane voice graph to four independent four-lane graphs.
// This runs before parameter conversion, so cloned states, banks and MIDI
// mappings all go through the same audited oscillator translation.
unsigned expand_unison(PatchData &p) {
	constexpr std::string_view helper = "SurgeXTRack:SurgeXTUnisonHelper",
							   expander = "SurgeXTRack:SurgeXTUnisonHelperCVExpander";
	// Hub's expander-tag heuristic also records the ordinary module to the
	// right of the final CV expander. That adjacency has no Unison semantics.
	std::erase_if(p.expanders, [&](auto const &e) {
		return e.left_module_id < p.module_slugs.size() && e.right_module_id < p.module_slugs.size() &&
			   std::string_view(p.module_slugs[e.left_module_id].c_str()) == expander &&
			   std::string_view(p.module_slugs[e.right_module_id].c_str()).starts_with("SurgeXTRack:") &&
			   std::string_view(p.module_slugs[e.right_module_id].c_str()) != expander;
	});
	const auto original = p;
	if (std::none_of(original.module_slugs.begin(), original.module_slugs.end(), [&](auto const &s) {
			return std::string_view(s.c_str()) == helper || std::string_view(s.c_str()) == expander;
		}))
		return 0;
	auto slug = [&](unsigned m) -> std::string_view {
		require(m < original.module_slugs.size(), "Unison cable references missing module");
		return original.module_slugs[m].c_str();
	};
	auto check_bank = [&](Jack j) {
		if (slug(j.module_id) == helper)
			require(j.jack_id < 3, "Higher Rack Unison pitch banks exceed native capacity");
		if (slug(j.module_id) == expander)
			require(j.jack_id == 0 || j.jack_id == 4, "Higher Rack Unison expander banks exceed native capacity");
	};
	for (auto const &c : original.int_cables)
		check_bank(c.out);
	for (auto const &c : original.mapped_outs)
		check_bank(c.out);
	std::map<unsigned, unsigned> left, right, owners;
	for (auto const &e : original.expanders)
		if (slug(e.left_module_id) == helper || slug(e.left_module_id) == expander ||
			slug(e.right_module_id) == expander)
		{
			require((slug(e.left_module_id) == helper || slug(e.left_module_id) == expander) &&
						slug(e.right_module_id) == expander,
					"Unison expander must follow its helper or another Unison expander");
			require(left.emplace(e.right_module_id, e.left_module_id).second &&
						right.emplace(e.left_module_id, e.right_module_id).second,
					"Ambiguous Unison expander chain");
		}
	for (unsigned id = 0; id < original.module_slugs.size(); ++id)
		if (slug(id) == expander) {
			std::set<unsigned> seen;
			unsigned m = id;
			while (left.contains(m)) {
				require(seen.insert(m).second, "Cyclic Unison expander chain");
				m = left.at(m);
			}
			if (slug(m) == helper)
				owners[id] = m;
		}
	unsigned added = 0;
	std::set<unsigned> claimed;
	for (unsigned h = 0; h < original.module_slugs.size(); ++h)
		if (slug(h) == helper) {
			std::optional<Jack> osc;
			for (auto const &c : original.int_cables)
				for (auto j : c.ins)
					if (j.module_id == h && j.jack_id >= 1 && j.jack_id <= 4) {
						require(
							j.jack_id == 1,
							"Unison import currently supports the first 16-lane Rack oscillator bank; higher Rack banks exceed native capacity");
						require(!osc, "Several returns feed Unison; explicit summing is required");
						osc = c.out;
					}
			for (auto const &c : original.mapped_ins)
				for (auto j : c.ins)
					require(j.module_id != h || j.jack_id < 1 || j.jack_id > 4,
							"Unison audio returns need an internal oscillator loop for automatic bank splitting");
			if (!osc)
				continue;
			std::set<unsigned> group, oscillators, pitched;
			auto effect = [&](unsigned m) {
				return slug(m).starts_with("SurgeXTRack:") ? effect_spec(std::string(slug(m).substr(12))) : nullptr;
			};
			std::function<void(unsigned)> visit = [&](unsigned m) {
				if (group.contains(m))
					return;
				const bool vco = slug(m).starts_with("SurgeXTRack:SurgeXTOSC");
				auto *fx = effect(m);
				require(vco || slug(m) == "SurgeXTRack:SurgeXTVCF" || slug(m) == "SurgeXTRack:SurgeXTWaveshaper" || fx,
						"Unison return graph contains an unsupported processing module");
				if (fx) {
					Json state(json_object(), json_decref);
					unsigned count = 0;
					for (auto const &s : original.module_states)
						if (s.module_id == m) {
							json_error_t error{};
							require(++count == 1, "Duplicate Unison effect state");
							state.reset(
								json_loadb(s.state_data.data(), s.state_data.size(), JSON_REJECT_DUPLICATES, &error));
							require(bool(state), "Invalid Unison effect state");
						}
					require(
						json_bool(json_object_get(state.get(), "modulespecific"), "polyphonicMode", fx->poly_default),
						"Enable polyphonic mode on effects inside a Unison return graph");
				}
				group.insert(m);
				if (vco) {
					oscillators.insert(m);
					return;
				}
				for (auto const &c : original.int_cables)
					for (auto j : c.ins)
						if (j.module_id == m && j.jack_id < 2) {
							if (owners.contains(c.out.module_id) && owners.at(c.out.module_id) == h)
								continue;
							visit(c.out.module_id);
						}
			};
			visit(osc->module_id);
			auto global_input = [&](Jack j) {
				return (oscillators.contains(j.module_id) && j.jack_id == 6) || (effect(j.module_id) && j.jack_id == 4);
			};
			std::set<Key> global_only_outputs, local_outputs;
			for (auto const &c : original.int_cables)
				if (group.contains(c.out.module_id))
					for (auto j : c.ins) {
						(global_input(j) ? global_only_outputs : local_outputs).emplace(c.out.module_id, c.out.jack_id);
					}
			for (auto key : local_outputs)
				global_only_outputs.erase(key);
			for (auto m : group)
				require(claimed.insert(m).second, "A processing module cannot belong to several Unison helpers");
			for (auto const &c : original.int_cables) {
				if (c.out.module_id == h && c.out.jack_id >= 2) {
					require(c.out.jack_id == 2, "Unison pitch bank is outside native capacity");
					for (auto j : c.ins) {
						require(oscillators.contains(j.module_id) && j.jack_id == 0,
								"Unison pitch bank must connect to oscillators in its return graph");
						pitched.insert(j.module_id);
					}
				}
				if (group.contains(c.out.module_id))
					for (auto j : c.ins)
						require(group.contains(j.module_id) || (j.module_id == h && j.jack_id == 1),
								"Unison voice output fanout outside its return graph needs separate routing");
				for (auto j : c.ins)
					if (group.contains(j.module_id)) {
						if (group.contains(c.out.module_id) ||
							(c.out.module_id == h && c.out.jack_id == 2 && j.jack_id == 0))
							continue;
						require(owners.contains(c.out.module_id) && owners.at(c.out.module_id) == h &&
									(c.out.jack_id == 0 || c.out.jack_id == 4),
								"Route external Unison voice CV through its CV expander before exporting");
					}
			}
			require(!oscillators.empty() && pitched == oscillators,
					"Unison return oscillators need pitch cables from their helper");
			for (auto const &c : original.mapped_outs)
				require(!group.contains(c.out.module_id) && (c.out.module_id != h || c.out.jack_id < 2),
						"Unison voice/pitch bank physical output mapping requires separate routing");
			for (auto const &e : original.expanders)
				require(!group.contains(e.left_module_id) && !group.contains(e.right_module_id),
						"Expanders attached to Unison voice processors cannot be cloned");
			for (auto const &light : original.mapped_lights)
				require(!group.contains(light.module_id), "Unison voice light mappings cannot be cloned");
			std::map<unsigned, std::array<unsigned, 4>> copies;
			for (auto m : group)
				copies[m][0] = m;
			int row = 0;
			for (auto const &pos : p.module_positions)
				row = std::max(row, int(pos.y) + 1);
			require(row < 32767, "Unison oscillator placement exceeds patch coordinates");
			for (unsigned bank = 1; bank < 4; ++bank) {
				unsigned column = 0;
				for (auto v : group) {
					require(p.module_slugs.size() < 64, "Unison bank splitting exceeds the 64-module patch limit");
					auto id = unsigned(p.module_slugs.size());
					copies[v][bank] = id;
					p.module_slugs.push_back(original.module_slugs[v]);
					++added;
					p.module_aliases.push_back({uint16_t(id),
												("Surge " + std::to_string(h) + " bank " + std::to_string(bank + 1) +
												 " module " + std::to_string(v))
													.c_str()});
					if (!original.module_positions.empty())
						p.module_positions.push_back(
							{uint16_t(id), int16_t(((bank - 1) * group.size() + column++) * 20), int16_t(row)});
					for (auto s : original.static_knobs)
						if (s.module_id == v) {
							s.module_id = id;
							p.static_knobs.push_back(s);
						}
					for (auto s : original.module_states)
						if (s.module_id == v) {
							s.module_id = id;
							p.module_states.push_back(s);
						}
					if (original.is_module_bypassed(v))
						p.bypassed_modules.push_back(id);
					for (unsigned b = 0; b < original.knob_sets.size(); ++b)
						for (auto m : original.knob_sets[b].set)
							if (m.module_id == v) {
								m.module_id = id;
								p.knob_sets[b].set.push_back(m);
							}
					for (auto m : original.midi_maps.set)
						if (m.module_id == v) {
							m.module_id = id;
							p.midi_maps.set.push_back(m);
						}
				}
				for (auto const &c : original.int_cables) {
					for (auto j : c.ins)
						if (group.contains(j.module_id) ||
							(group.contains(c.out.module_id) && j.module_id == h && j.jack_id == 1))
						{
							auto out = c.out;
							auto dest = j;
							if (group.contains(out.module_id)) {
								if (!global_input(j))
									out.module_id = copies.at(out.module_id)[bank];
							} else if (out.module_id == h)
								out.jack_id = 2 + bank;
							else {
								// Oscillator audio and FX clocks use the first source lane
								// globally; all other expander destinations follow banks.
								if (!global_input(j))
									out.jack_id += bank;
							}
							if (group.contains(j.module_id))
								dest.module_id = copies.at(j.module_id)[bank];
							else
								dest.jack_id = 1 + bank;
							p.int_cables.push_back({out, {dest}, c.color});
						}
				}
			}
			// Global first-lane consumers keep their original source. Terminate
			// otherwise-unused copied outputs so connected-output-dependent stereo
			// behavior remains identical in every bank.
			unsigned terminator_column = 0;
			for (unsigned bank = 1; bank < 4; ++bank)
				for (auto [m, port] : global_only_outputs) {
					require(p.module_slugs.size() < 64, "Unison output termination exceeds the 64-module patch limit");
					unsigned sink = p.module_slugs.size();
					p.module_slugs.emplace_back("4msCompany:Atvert2");
					++added;
					p.static_knobs.push_back({uint16_t(sink), 0, .5f});
					p.static_knobs.push_back({uint16_t(sink), 1, .5f});
					p.module_aliases.push_back({uint16_t(sink), "Unison output termination"});
					p.int_cables.push_back(
						{{uint16_t(copies.at(m)[bank]), uint16_t(port)}, {{uint16_t(sink), 0}}, 0xfc80});
					if (!original.module_positions.empty()) {
						require(row < 32766, "Unison termination placement exceeds patch coordinates");
						p.module_positions.push_back(
							{uint16_t(sink), int16_t(terminator_column++ * 4), int16_t(row + 1)});
					}
				}
			for (unsigned i = 0; i < original.mapped_ins.size(); ++i)
				for (auto j : original.mapped_ins[i].ins)
					if (group.contains(j.module_id)) {
						require(!MetaModule::Midi::is_midi_poly_cable(original.mapped_ins[i].panel_jack_id) &&
									!MetaModule::Midi::is_midi_poly5_8_cable(original.mapped_ins[i].panel_jack_id),
								"Route polyphonic MIDI oscillator CV through the Unison CV expander");
						require(!(oscillators.contains(j.module_id) && j.jack_id == 0),
								"Unison oscillator pitch cannot have a second source");
						require(oscillators.contains(j.module_id) || j.jack_id >= 2,
								"Use an expander for external audio inside a polyphonic Unison graph");
						for (unsigned bank = 1; bank < 4; ++bank)
							p.mapped_ins[i].ins.push_back({uint16_t(copies.at(j.module_id)[bank]), j.jack_id});
					}
			// Higher original expander banks represent lanes 16..63, not the new
			// four-lane banks. Reject these before adding replacement connections.
			for (auto const &c : original.int_cables)
				if (owners.contains(c.out.module_id) && owners.at(c.out.module_id) == h)
					require(c.out.jack_id == 0 || c.out.jack_id == 4,
							"Higher Rack Unison expander banks exceed native capacity");
		}
	return added;
}
} // namespace
Report convert(PatchData &patch) {
	auto candidate = patch;
	auto copies = expand_unison(candidate);
	auto report = Plan(candidate).run();
	report.helper_modules += copies;
	if (copies)
		report.notes.push_back(
			"Unison voice processing was split into four-lane copies. All knob banks, MIDI targets and module states were copied; hardware resource use remains to be measured.");
	patch = std::move(candidate);
	return report;
}
} // namespace SurgeNativeExport
