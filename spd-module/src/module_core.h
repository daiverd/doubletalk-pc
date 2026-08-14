#ifndef DOUBLETALK_SPD_MODULE_CORE_H
#define DOUBLETALK_SPD_MODULE_CORE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace doubletalk_spd {

struct ModuleConfig {
	std::string rom_path;
	std::uint32_t lowpass_hz = 3800;
	std::uint32_t output_rate = 48000;
};

struct ParseResult {
	ModuleConfig config;
	std::string error;

	[[nodiscard]] bool ok() const { return error.empty(); }
};

ParseResult parse_config_text(std::string_view text);
ParseResult parse_config_file(const std::string &path);

struct SpeechSettings {
	int rate = 0;
	int pitch = 0;
	int volume = 0;
	int voice_id = 0;
};

int map_rate(int value);
int map_pitch(int value);
int map_volume(int value);
int voice_id_from_name(std::string_view name);
std::string_view voice_name(int voice_id);
std::string build_command_prefix(const SpeechSettings &settings);

struct IndexMarkMapping {
	std::uint8_t number;
	std::string name;
};

struct ConvertedText {
	std::string bytes;
	std::vector<IndexMarkMapping> marks;
	std::string error;

	[[nodiscard]] bool ok() const { return error.empty(); }
};

ConvertedText convert_text(std::string_view text);

} // namespace doubletalk_spd

#endif
