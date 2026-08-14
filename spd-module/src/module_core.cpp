#include "module_core.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace doubletalk_spd {
namespace {

constexpr std::array<std::string_view, 8> VOICE_NAMES = {
	"Perfect Paul", "Vader", "Big Bob", "Precise Pete",
	"Ricochet", "Biff", "Skip", "Robo Robert",
};

struct Tokens {
	std::vector<std::string> values;
	std::string error;
};

Tokens tokenize(std::string_view line)
{
	Tokens result;
	std::size_t pos = 0;

	while (pos < line.size()) {
		while (pos < line.size()
		       && std::isspace(static_cast<unsigned char>(line[pos])))
			++pos;
		if (pos == line.size() || line[pos] == '#')
			break;

		std::string token;
		if (line[pos] == '"') {
			++pos;
			while (pos < line.size() && line[pos] != '"') {
				if (line[pos] == '\\' && pos + 1 < line.size()
				    && (line[pos + 1] == '"' || line[pos + 1] == '\\'))
					++pos;
				token.push_back(line[pos++]);
			}
			if (pos == line.size()) {
				result.error = "unterminated quoted string";
				return result;
			}
			++pos;
			if (pos < line.size()
			    && !std::isspace(static_cast<unsigned char>(line[pos]))
			    && line[pos] != '#') {
				result.error = "unexpected text after quoted string";
				return result;
			}
		} else {
			while (pos < line.size()
			       && !std::isspace(static_cast<unsigned char>(line[pos]))
			       && line[pos] != '#')
				token.push_back(line[pos++]);
		}
		result.values.push_back(std::move(token));
	}
	return result;
}

ParseResult fail(std::size_t line, const std::string &message)
{
	return {{}, "line " + std::to_string(line) + ": " + message};
}

std::string_view trim(std::string_view value)
{
	while (!value.empty()
	       && std::isspace(static_cast<unsigned char>(value.front())))
		value.remove_prefix(1);
	while (!value.empty()
	       && std::isspace(static_cast<unsigned char>(value.back())))
		value.remove_suffix(1);
	return value;
}

bool is_mark_tag(std::string_view tag)
{
	tag = trim(tag);
	return tag.starts_with("mark")
	       && (tag.size() == 4
	           || std::isspace(static_cast<unsigned char>(tag[4]))
	           || tag[4] == '/');
}

bool looks_like_tag_start(std::string_view text, std::size_t pos)
{
	if (pos + 1 >= text.size())
		return false;
	const unsigned char next = static_cast<unsigned char>(text[pos + 1]);
	return std::isalpha(next) || next == '/' || next == '!' || next == '?';
}

std::string mark_name(std::string_view tag)
{
	std::size_t pos = 4;
	while (pos < tag.size()) {
		while (pos < tag.size()
		       && std::isspace(static_cast<unsigned char>(tag[pos])))
			++pos;
		if (pos == tag.size() || tag[pos] == '/')
			break;

		const std::size_t name_start = pos;
		while (pos < tag.size()
		       && (std::isalnum(static_cast<unsigned char>(tag[pos]))
		           || tag[pos] == '_' || tag[pos] == '-'))
			++pos;
		const auto attribute = tag.substr(name_start, pos - name_start);
		while (pos < tag.size()
		       && std::isspace(static_cast<unsigned char>(tag[pos])))
			++pos;
		if (pos == tag.size() || tag[pos] != '=')
			return {};
		++pos;
		while (pos < tag.size()
		       && std::isspace(static_cast<unsigned char>(tag[pos])))
			++pos;
		if (pos == tag.size() || (tag[pos] != '"' && tag[pos] != '\''))
			return {};
		const char quote = tag[pos++];
		const std::size_t value_start = pos;
		const std::size_t value_end = tag.find(quote, pos);
		if (value_end == std::string_view::npos)
			return {};
		if (attribute == "name")
			return std::string(tag.substr(value_start, value_end - value_start));
		pos = value_end + 1;
	}
	return {};
}

} // namespace

ParseResult parse_config_text(std::string_view text)
{
	ParseResult result;
	bool saw_rom_path = false;
	std::size_t line_number = 0;
	std::size_t start = 0;

	while (start <= text.size()) {
		++line_number;
		const std::size_t end = text.find('\n', start);
		const auto line = text.substr(
			start, end == std::string_view::npos ? text.size() - start
			                                    : end - start);
		auto tokens = tokenize(line);
		if (!tokens.error.empty())
			return fail(line_number, tokens.error);
		if (!tokens.values.empty()) {
			if (tokens.values.size() != 2)
				return fail(line_number, "expected a directive and one value");

			const auto &name = tokens.values[0];
			const auto &value = tokens.values[1];
			if (name == "DoubleTalkRomPath") {
				if (value.empty())
					return fail(line_number, "DoubleTalkRomPath may not be empty");
				result.config.rom_path = value;
				saw_rom_path = true;
			} else if (name == "DoubleTalkLowPassHz") {
				std::uint32_t hz = 0;
				const auto parsed = std::from_chars(
					value.data(), value.data() + value.size(), hz);
				if (parsed.ec != std::errc()
				    || parsed.ptr != value.data() + value.size())
					return fail(line_number,
					            "DoubleTalkLowPassHz must be an integer");
				if (hz < 500 || hz > 5000)
					return fail(line_number,
					            "DoubleTalkLowPassHz must be between 500 and 5000");
				result.config.lowpass_hz = hz;
			} else if (name == "DoubleTalkOutputRate") {
				std::uint32_t rate = 0;
				const auto parsed = std::from_chars(
					value.data(), value.data() + value.size(), rate);
				if (parsed.ec != std::errc()
				    || parsed.ptr != value.data() + value.size())
					return fail(line_number,
					            "DoubleTalkOutputRate must be an integer");
				if (rate < 8000 || rate > 192000)
					return fail(
						line_number,
						"DoubleTalkOutputRate must be between 8000 and 192000");
				result.config.output_rate = rate;
			} else {
				return fail(line_number, "unknown directive " + name);
			}
		}

		if (end == std::string_view::npos)
			break;
		start = end + 1;
	}

	if (!saw_rom_path)
		result.error = "DoubleTalkRomPath is required";
	return result;
}

ParseResult parse_config_file(const std::string &path)
{
	if (path.empty())
		return {{}, "module configuration path is required"};

	std::ifstream input(path);
	if (!input)
		return {{}, "cannot open module configuration: " + path};

	std::ostringstream contents;
	contents << input.rdbuf();
	if (!input.good() && !input.eof())
		return {{}, "cannot read module configuration: " + path};
	return parse_config_text(contents.str());
}

int map_rate(int value)
{
	value = std::clamp(value, -100, 100);
	return std::min(9, (value + 100) * 10 / 200);
}

int map_pitch(int value)
{
	value = std::clamp(value, -100, 100);
	return ((value + 100) * 99 + 100) / 200;
}

int map_volume(int value)
{
	value = std::clamp(value, -100, 100);
	return std::min(9, (value + 100) * 10 / 200);
}

int voice_id_from_name(std::string_view name)
{
	for (std::size_t i = 0; i < VOICE_NAMES.size(); ++i) {
		if (name == VOICE_NAMES[i])
			return static_cast<int>(i);
	}
	return -1;
}

std::string_view voice_name(int voice_id)
{
	if (voice_id < 0 || voice_id >= static_cast<int>(VOICE_NAMES.size()))
		return {};
	return VOICE_NAMES[static_cast<std::size_t>(voice_id)];
}

std::string build_command_prefix(const SpeechSettings &settings)
{
	const int voice = std::clamp(settings.voice_id, 0, 7);
	return "\x01" + std::to_string(voice) + "O"
	       + "\x01" "14B"
	       + "\x01" + std::to_string(map_rate(settings.rate)) + "S"
	       + "\x01" + std::to_string(map_pitch(settings.pitch)) + "P"
	       + "\x01" + std::to_string(map_volume(settings.volume)) + "V";
}

ConvertedText convert_text(std::string_view text)
{
	ConvertedText result;

	for (std::size_t pos = 0; pos < text.size();) {
		if (text[pos] == '<' && looks_like_tag_start(text, pos)) {
			const std::size_t end = text.find('>', pos + 1);
			if (end == std::string_view::npos) {
				result.error = "unterminated SSML tag";
				return result;
			}
			const auto tag = trim(text.substr(pos + 1, end - pos - 1));
			if (is_mark_tag(tag)) {
				const std::string name = mark_name(tag);
				if (name.empty()) {
					result.error = "SSML mark requires a non-empty quoted name";
					return result;
				}
				if (result.marks.size() >= 100) {
					result.error = "an utterance may contain at most 100 index marks";
					return result;
				}
				const auto number =
					static_cast<std::uint8_t>(result.marks.size());
				result.bytes += "\x01" + std::to_string(number) + "I";
				result.marks.push_back({number, name});
			}
			pos = end + 1;
			continue;
		}

		if (text[pos] == '&') {
			const std::size_t end = text.find(';', pos + 1);
			if (end != std::string_view::npos) {
				const auto entity = text.substr(pos, end - pos + 1);
				char decoded = '\0';
				if (entity == "&amp;")
					decoded = '&';
				else if (entity == "&lt;")
					decoded = '<';
				else if (entity == "&gt;")
					decoded = '>';
				else if (entity == "&quot;")
					decoded = '"';
				else if (entity == "&apos;")
					decoded = '\'';
				if (decoded != '\0') {
					result.bytes.push_back(decoded);
					pos = end + 1;
					continue;
				}
			}
		}

		const unsigned char value = static_cast<unsigned char>(text[pos++]);
		result.bytes.push_back(value >= 0x20 && value <= 0x7e
			                       ? static_cast<char>(value)
			                       : ' ');
	}
	return result;
}

} // namespace doubletalk_spd
