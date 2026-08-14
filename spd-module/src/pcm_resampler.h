#ifndef DOUBLETALK_SPD_PCM_RESAMPLER_H
#define DOUBLETALK_SPD_PCM_RESAMPLER_H

#include <cstdint>
#include <span>
#include <vector>

struct SpeexResamplerState_;

namespace doubletalk_spd {

class PcmResampler {
public:
	PcmResampler(std::uint32_t input_rate, std::uint32_t output_rate);
	~PcmResampler();

	PcmResampler(const PcmResampler &) = delete;
	PcmResampler &operator=(const PcmResampler &) = delete;

	std::vector<std::int16_t> process(
		std::span<const std::int16_t> input);
	std::vector<std::int16_t> flush();
	void reset();

	[[nodiscard]] std::uint32_t input_rate() const { return input_rate_; }
	[[nodiscard]] std::uint32_t output_rate() const { return output_rate_; }

private:
	std::uint32_t input_rate_;
	std::uint32_t output_rate_;
	SpeexResamplerState_ *state_ = nullptr;
};

} // namespace doubletalk_spd

#endif
