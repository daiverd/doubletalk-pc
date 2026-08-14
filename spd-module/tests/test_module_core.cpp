#include "module_core.h"
#include "pcm_resampler.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		std::exit(1);
	}
}

void test_parses_configuration()
{
	const auto result = doubletalk_spd::parse_config_text(
		"# DoubleTalk module\n"
		"DoubleTalkRomPath \"/tmp/double talk.bin\"\n"
		"DoubleTalkLowPassHz 4800\n");

	check(result.ok(), "valid configuration should parse");
	check(result.config.rom_path == "/tmp/double talk.bin",
	      "quoted ROM path should preserve spaces");
	check(result.config.lowpass_hz == 4800,
	      "configured low-pass should be retained");
	check(result.config.output_rate == 48000,
	      "output should default to a tunnel-friendly 48 kHz");

	const auto defaults = doubletalk_spd::parse_config_text(
		"DoubleTalkRomPath /tmp/rom.bin\n");
	check(defaults.ok() && defaults.config.lowpass_hz == 3800,
	      "low-pass should default to the emulator's MAME-matching 3.8 kHz");

	const auto rate = doubletalk_spd::parse_config_text(
		"DoubleTalkRomPath /tmp/rom.bin\n"
		"DoubleTalkOutputRate 44100\n");
	check(rate.ok() && rate.config.output_rate == 44100,
	      "configured output sample rate should be retained");
}

void test_rejects_invalid_configuration()
{
	using namespace doubletalk_spd;

	check(!parse_config_text("DoubleTalkLowPassHz 3000\n").ok(),
	      "ROM path should be required");
	check(!parse_config_text("DoubleTalkRomPath /tmp/rom.bin extra\n").ok(),
	      "trailing configuration arguments should be rejected");
	check(!parse_config_text("DoubleTalkRomPath /tmp/rom.bin\nUnknown x\n").ok(),
	      "unknown directives should be rejected");
	check(!parse_config_text(
		       "DoubleTalkRomPath /tmp/rom.bin\nDoubleTalkLowPassHz 499\n")
		       .ok(),
	      "out-of-range low-pass values should be rejected");
	check(!parse_config_text("DoubleTalkRomPath \"unterminated\n").ok(),
	      "unterminated quoted paths should be rejected");
}

void test_maps_settings_and_voices()
{
	using namespace doubletalk_spd;

	check(map_rate(-100) == 0, "minimum rate should map to speed 0");
	check(map_rate(0) == 5, "middle rate should map to speed 5");
	check(map_rate(100) == 9, "maximum rate should map to speed 9");
	check(map_pitch(-100) == 0, "minimum pitch should map to pitch 0");
	check(map_pitch(0) == 50, "middle pitch should map to pitch 50");
	check(map_pitch(100) == 99, "maximum pitch should map to pitch 99");
	check(map_volume(0) == 5, "middle volume should map to volume 5");

	const char *names[] = {
		"Perfect Paul", "Vader", "Big Bob", "Precise Pete",
		"Ricochet", "Biff", "Skip", "Robo Robert",
	};
	for (int i = 0; i < 8; ++i)
		check(voice_id_from_name(names[i]) == i,
		      "voice name should map to its firmware voice");

	SpeechSettings settings;
	settings.voice_id = 3;
	settings.rate = 100;
	settings.pitch = -100;
	settings.volume = 0;
	check(build_command_prefix(settings)
		      == std::string(
			      "\x01" "3O\x01" "14B\x01" "9S\x01" "0P\x01" "5V"),
	      "prefix should select voice, preserve leading zeros, then set controls");
}

void test_converts_ssml_and_index_marks()
{
	using namespace doubletalk_spd;

	const auto converted = convert_text(
		"<speak>Hello &amp; <emphasis>world</emphasis>"
		"<mark name=\"checkpoint\"/>!\n</speak>");
	check(converted.ok(), "valid basic SSML should convert");
	check(converted.bytes
		      == std::string("Hello & world\x01" "0I! "),
	      "tags should be stripped, entities decoded, and mark encoded");
	check(converted.marks.size() == 1,
	      "one firmware marker mapping should be recorded");
	check(converted.marks[0].number == 0,
	      "first firmware marker should use number zero");
	check(converted.marks[0].name == "checkpoint",
	      "original Speech Dispatcher mark name should be retained");

	const auto controls = convert_text(std::string("a\001b\xc3\xa9", 5));
	check(controls.bytes == "a b  ",
	      "control and non-ASCII bytes should become spaces");

	const auto comparison = convert_text("2 < 3 > 1");
	check(comparison.ok() && comparison.bytes == "2 < 3 > 1",
	      "plain less-than and greater-than characters should remain text");
}

void test_rejects_invalid_or_excess_marks()
{
	using namespace doubletalk_spd;

	check(!convert_text("<mark/>").ok(),
	      "marks without names should be rejected");

	std::string too_many_marks;
	for (int i = 0; i < 101; ++i)
		too_many_marks += "<mark name=\"m\"/>";
	check(!convert_text(too_many_marks).ok(),
	      "more than 100 marks should be rejected");
}

void test_resampling_is_chunk_invariant()
{
	using doubletalk_spd::PcmResampler;

	std::vector<std::int16_t> input(10504);
	for (std::size_t i = 0; i < input.size(); ++i) {
		const double phase = 2.0 * 3.14159265358979323846 * 440.0
		                     * static_cast<double>(i) / 10504.0;
		input[i] = static_cast<std::int16_t>(std::lround(std::sin(phase) * 12000));
	}

	PcmResampler whole(10504, 48000);
	auto expected = whole.process(input);
	auto whole_tail = whole.flush();
	expected.insert(expected.end(), whole_tail.begin(), whole_tail.end());
	if (expected.size() < 47990 || expected.size() > 48010) {
		std::cerr << "resampler produced " << expected.size()
		          << " samples for one second\n";
		check(false,
		      "one second of native PCM should produce about one second at 48 kHz");
	}

	PcmResampler chunked(10504, 48000);
	std::vector<std::int16_t> actual;
	for (std::size_t offset = 0; offset < input.size();) {
		const std::size_t count = std::min<std::size_t>(317, input.size() - offset);
		auto part = chunked.process(
			std::span<const std::int16_t>(input.data() + offset, count));
		actual.insert(actual.end(), part.begin(), part.end());
		offset += count;
	}
	auto chunked_tail = chunked.flush();
	actual.insert(actual.end(), chunked_tail.begin(), chunked_tail.end());
	check(actual == expected,
	      "resampling output should not depend on synthesis chunk boundaries");
}

} // namespace

int main()
{
	test_parses_configuration();
	test_rejects_invalid_configuration();
	test_maps_settings_and_voices();
	test_converts_ssml_and_index_marks();
	test_rejects_invalid_or_excess_marks();
	test_resampling_is_chunk_invariant();
	std::cout << "module_core tests passed\n";
	return 0;
}
