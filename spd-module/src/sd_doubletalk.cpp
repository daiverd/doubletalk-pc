#include "module_core.h"
#include "pcm_resampler.h"

#include "dtalk.h"

#include <spd_module_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

constexpr std::size_t ROM_SIZE = 512 * 1024;
constexpr std::size_t AUDIO_CHUNK_SAMPLES = 2048;

doubletalk_spd::ModuleConfig module_config_values;
doubletalk_spd::SpeechSettings speech_settings;
dtalk *synth;
std::unique_ptr<doubletalk_spd::PcmResampler> resampler;
std::atomic<bool> stop_requested{false};
std::uint64_t samples_produced;

bool parse_setting(const char *value, int &output)
{
	if (value == nullptr)
		return false;
	const std::string_view text(value);
	int parsed = 0;
	const auto result =
		std::from_chars(text.data(), text.data() + text.size(), parsed);
	if (result.ec != std::errc() || result.ptr != text.data() + text.size())
		return false;
	output = std::clamp(parsed, -100, 100);
	return true;
}

void send_audio(const std::int16_t *samples, std::size_t count)
{
	if (count == 0 || stop_requested.load())
		return;
	AudioTrack track{
		.bits = 16,
		.num_channels = 1,
		.sample_rate = static_cast<int>(module_config_values.output_rate),
		.num_samples = static_cast<int>(count),
		.samples = const_cast<short *>(samples),
	};
	module_tts_output_server(&track, SPD_AUDIO_LE);
}

void send_native_audio(std::span<const std::int16_t> samples)
{
	const auto converted = resampler->process(samples);
	send_audio(converted.data(), converted.size());
}

} // namespace

int module_config(const char *configfile)
{
	const auto parsed =
		doubletalk_spd::parse_config_file(configfile == nullptr ? "" : configfile);
	if (!parsed.ok()) {
		std::fprintf(stderr, "sd_doubletalk: %s\n", parsed.error.c_str());
		return -1;
	}
	module_config_values = parsed.config;
	return 0;
}

int module_init(char **message)
{
	module_audio_set_server();

	std::ifstream rom_file(module_config_values.rom_path, std::ios::binary);
	if (!rom_file) {
		const std::string error =
			"cannot open DoubleTalk ROM: " + module_config_values.rom_path;
		*message = ::strdup(error.c_str());
		return -1;
	}
	std::vector<std::uint8_t> rom{
		std::istreambuf_iterator<char>(rom_file),
		std::istreambuf_iterator<char>(),
	};
	if (rom_file.bad() || rom.size() != ROM_SIZE) {
		const std::string error =
			"DoubleTalk ROM must be exactly 524288 bytes: "
			+ module_config_values.rom_path;
		*message = ::strdup(error.c_str());
		return -1;
	}

	synth = dtalk_create(rom.data(), rom.size());
	if (synth == nullptr) {
		*message = ::strdup("dtalk_create rejected the configured ROM");
		return -1;
	}
	dtalk_set_lowpass_hz(synth, module_config_values.lowpass_hz);
	try {
		resampler = std::make_unique<doubletalk_spd::PcmResampler>(
			static_cast<std::uint32_t>(dtalk_sample_rate(synth)),
			module_config_values.output_rate);
	} catch (const std::exception &error) {
		const std::string error_message =
			"cannot configure DoubleTalk audio output: "
			+ std::string(error.what());
		dtalk_destroy(synth);
		synth = nullptr;
		*message = ::strdup(error_message.c_str());
		return -1;
	}
	samples_produced = 0;
	*message = ::strdup("DoubleTalk PC emulator initialized");
	return 0;
}

SPDVoice **module_list_voices()
{
	auto **voices = static_cast<SPDVoice **>(
		std::calloc(9, sizeof(SPDVoice *)));
	if (voices == nullptr)
		return nullptr;

	for (int id = 0; id < 8; ++id) {
		voices[id] = static_cast<SPDVoice *>(std::calloc(1, sizeof(SPDVoice)));
		if (voices[id] == nullptr)
			return voices;
		const auto name = doubletalk_spd::voice_name(id);
		voices[id]->name = ::strndup(name.data(), name.size());
		voices[id]->language = ::strdup("en-US");
		voices[id]->variant = nullptr;
	}
	return voices;
}

int module_set(const char *variable, const char *value)
{
	if (variable == nullptr)
		return -1;

	if (std::strcmp(variable, "rate") == 0)
		return parse_setting(value, speech_settings.rate) ? 0 : -1;
	if (std::strcmp(variable, "pitch") == 0)
		return parse_setting(value, speech_settings.pitch) ? 0 : -1;
	if (std::strcmp(variable, "volume") == 0)
		return parse_setting(value, speech_settings.volume) ? 0 : -1;
	if (std::strcmp(variable, "synthesis_voice") == 0) {
		if (value == nullptr || std::strcmp(value, "NULL") == 0)
			return 0;
		const int id = doubletalk_spd::voice_id_from_name(value);
		if (id < 0)
			return -1;
		speech_settings.voice_id = id;
		return 0;
	}

	const char *accepted_noops[] = {
		"voice", "language", "pitch_range", "punctuation_mode",
		"spelling_mode", "cap_let_recogn",
	};
	for (const char *name : accepted_noops) {
		if (std::strcmp(variable, name) == 0)
			return 0;
	}
	return -1;
}

int module_audio_set(const char *variable, const char *value)
{
	if (variable == nullptr || value == nullptr)
		return -1;
	return std::strcmp(variable, "audio_output_method") == 0
		       && std::strcmp(value, "server") == 0
	           ? 0
	           : -1;
}

int module_audio_init(char **status)
{
	if (status != nullptr)
		*status = ::strdup("Speech Dispatcher server audio");
	return 0;
}

int module_loglevel_set(const char *, const char *)
{
	return 0;
}

int module_debug(int, const char *)
{
	return 0;
}

int module_loop()
{
	return module_process(STDIN_FILENO, 1);
}

void module_speak_sync(const char *data, std::size_t bytes,
	                   SPDMessageType)
{
	if (synth == nullptr || data == nullptr || bytes == 0) {
		module_speak_error();
		return;
	}

	auto converted =
		doubletalk_spd::convert_text(std::string_view(data, bytes));
	if (!converted.ok()) {
		std::fprintf(stderr, "sd_doubletalk: cannot speak request: %s\n",
		             converted.error.c_str());
		module_speak_error();
		return;
	}

	std::array<std::string, 100> mark_names;
	for (const auto &mark : converted.marks)
		mark_names[mark.number] = mark.name;

	std::string request =
		doubletalk_spd::build_command_prefix(speech_settings);
	request += converted.bytes;
	request.push_back('\r');

	stop_requested.store(false);
	try {
		resampler->reset();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "sd_doubletalk: cannot reset audio output: %s\n",
		             error.what());
		module_speak_error();
		return;
	}
	module_speak_ok();
	module_report_event_begin();
	dtalk_queue(synth, request.data(), request.size());

	std::array<std::int16_t, AUDIO_CHUNK_SAMPLES> pcm{};
	std::vector<dtalk_index_mark> pending_marks;
	bool synthesis_failed = false;

	while (!stop_requested.load()) {
		const std::size_t count =
			dtalk_synth16(synth, pcm.data(), pcm.size());
		if (count == 0)
			break;

		std::array<dtalk_index_mark, 16> reached{};
		for (;;) {
			const std::size_t mark_count =
				dtalk_read_index_marks(synth, reached.data(), reached.size());
			pending_marks.insert(pending_marks.end(), reached.begin(),
			                     reached.begin() + mark_count);
			if (mark_count < reached.size())
				break;
		}
		std::stable_sort(pending_marks.begin(), pending_marks.end(),
		                 [](const auto &left, const auto &right) {
			                 return left.sample_pos < right.sample_pos;
		                 });

		const std::uint64_t chunk_start = samples_produced;
		const std::uint64_t chunk_end = chunk_start + count;
		std::size_t offset = 0;
		std::size_t reported = 0;
		for (const auto &mark : pending_marks) {
			if (mark.sample_pos > chunk_end)
				break;
			const std::uint64_t relative =
				mark.sample_pos > chunk_start
					? mark.sample_pos - chunk_start
					: 0;
			const std::size_t split =
				std::min(count, static_cast<std::size_t>(relative));
			try {
				send_native_audio(std::span(pcm).subspan(
					offset, split - offset));
			} catch (const std::exception &error) {
				std::fprintf(stderr,
				             "sd_doubletalk: audio resampling failed: %s\n",
				             error.what());
				synthesis_failed = true;
				break;
			}
			if (stop_requested.load())
				break;
			if (!mark_names[mark.value].empty())
				module_report_index_mark(mark_names[mark.value].c_str());
			offset = split;
			++reported;
		}
		if (synthesis_failed)
			break;
		if (!stop_requested.load()) {
			try {
				send_native_audio(
					std::span(pcm).subspan(offset, count - offset));
			} catch (const std::exception &error) {
				std::fprintf(stderr,
				             "sd_doubletalk: audio resampling failed: %s\n",
				             error.what());
				synthesis_failed = true;
			}
		}

		samples_produced += count;
		pending_marks.erase(pending_marks.begin(),
		                    pending_marks.begin() + reported);
	}

	if (!stop_requested.load() && !synthesis_failed) {
		try {
			const auto tail = resampler->flush();
			send_audio(tail.data(), tail.size());
		} catch (const std::exception &error) {
			std::fprintf(stderr,
			             "sd_doubletalk: cannot finish audio resampling: %s\n",
			             error.what());
			synthesis_failed = true;
		}
	}

	if (stop_requested.load() || synthesis_failed) {
		/*
		 * Keep every operation on the non-thread-safe dtalk handle on the
		 * synthesis path. In Speech Dispatcher 0.12 STOP is processed
		 * reentrantly by module_tts_output_server(), but this also remains
		 * safe if a future module basis invokes module_stop() on another
		 * thread.
		 */
		dtalk_stop(synth);
		module_report_event_stop();
	} else {
		module_report_event_end();
	}
}

std::size_t module_pause()
{
	module_stop();
	return 0;
}

int module_stop()
{
	stop_requested.store(true);
	return 0;
}

int module_close()
{
	resampler.reset();
	if (synth != nullptr) {
		dtalk_destroy(synth);
		synth = nullptr;
	}
	return 0;
}
