#include "pcm_resampler.h"

#include <speex/speex_resampler.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace doubletalk_spd {

PcmResampler::PcmResampler(std::uint32_t input_rate,
	                       std::uint32_t output_rate)
	: input_rate_(input_rate), output_rate_(output_rate)
{
	if (input_rate == 0 || output_rate == 0)
		throw std::invalid_argument("PCM sample rates must be nonzero");
	if (input_rate == output_rate)
		return;

	int error = RESAMPLER_ERR_SUCCESS;
	state_ = speex_resampler_init(
		1, input_rate, output_rate, SPEEX_RESAMPLER_QUALITY_DESKTOP, &error);
	if (state_ == nullptr || error != RESAMPLER_ERR_SUCCESS)
		throw std::runtime_error(
			"cannot initialize SpeexDSP resampler: "
			+ std::string(speex_resampler_strerror(error)));

	/*
	 * The DoubleTalk stream already begins with firmware-generated silence.
	 * Remove Speex's additional filter pre-roll so index timing and stop
	 * responsiveness are not shifted by a second startup delay.
	 */
	speex_resampler_skip_zeros(state_);
}

PcmResampler::~PcmResampler()
{
	if (state_ != nullptr)
		speex_resampler_destroy(state_);
}

std::vector<std::int16_t> PcmResampler::process(
	std::span<const std::int16_t> input)
{
	if (input.empty())
		return {};
	if (state_ == nullptr)
		return {input.begin(), input.end()};
	if (input.size() > std::numeric_limits<spx_uint32_t>::max())
		throw std::length_error("PCM input chunk is too large");

	std::vector<std::int16_t> output;
	std::size_t consumed = 0;
	while (consumed < input.size()) {
		const std::size_t remaining = input.size() - consumed;
		const auto estimated = static_cast<std::size_t>(
			std::ceil(static_cast<double>(remaining) * output_rate_
			          / input_rate_));
		std::vector<std::int16_t> buffer(estimated + 64);
		spx_uint32_t input_count = static_cast<spx_uint32_t>(remaining);
		spx_uint32_t output_count =
			static_cast<spx_uint32_t>(buffer.size());
		const int error = speex_resampler_process_int(
			state_, 0, input.data() + consumed, &input_count,
			buffer.data(), &output_count);
		if (error != RESAMPLER_ERR_SUCCESS)
			throw std::runtime_error(
				"SpeexDSP resampling failed: "
				+ std::string(speex_resampler_strerror(error)));
		if (input_count == 0 && output_count == 0)
			throw std::runtime_error("SpeexDSP resampler made no progress");

		output.insert(output.end(), buffer.begin(),
		              buffer.begin() + output_count);
		consumed += input_count;
	}
	return output;
}

std::vector<std::int16_t> PcmResampler::flush()
{
	if (state_ == nullptr)
		return {};
	const spx_uint32_t latency = speex_resampler_get_input_latency(state_);
	if (latency == 0)
		return {};
	std::vector<std::int16_t> silence(latency, 0);
	return process(silence);
}

void PcmResampler::reset()
{
	if (state_ == nullptr)
		return;
	const int error = speex_resampler_reset_mem(state_);
	if (error != RESAMPLER_ERR_SUCCESS)
		throw std::runtime_error(
			"cannot reset SpeexDSP resampler: "
			+ std::string(speex_resampler_strerror(error)));
	speex_resampler_skip_zeros(state_);
}

} // namespace doubletalk_spd
