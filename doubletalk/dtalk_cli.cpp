// license:BSD-3-Clause
// CLI harness for the standalone DoubleTalk PC board.
//
//   dtalk_cli <rom> trace [max_instructions]     - boot trace to stdout
//   dtalk_cli <rom> boot [cycles]                - run boot, report state
//   dtalk_cli <rom> say <text> <out.wav>         - synthesize to 8-bit WAV

#include "doubletalk_board.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <map>
#include <vector>

static std::vector<u8> read_file(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool write_wav_u8(const std::string &path, const std::vector<u8> &pcm, u32 rate)
{
	std::ofstream f(path, std::ios::binary);
	if (!f)
		return false;
	u32 data_size = u32(pcm.size());
	u32 byte_rate = rate;
	u8 hdr[44] = {
		'R','I','F','F', 0,0,0,0, 'W','A','V','E',
		'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
		0,0,0,0, 0,0,0,0, 1,0, 8,0,
		'd','a','t','a', 0,0,0,0
	};
	u32 riff_size = 36 + data_size;
	std::memcpy(hdr + 4, &riff_size, 4);
	std::memcpy(hdr + 24, &rate, 4);
	std::memcpy(hdr + 28, &byte_rate, 4);
	std::memcpy(hdr + 40, &data_size, 4);
	f.write(reinterpret_cast<char *>(hdr), sizeof(hdr));
	f.write(reinterpret_cast<const char *>(pcm.data()), pcm.size());
	return bool(f);
}

static void print_state(doubletalk_board &board)
{
	doubletalk_cpu &cpu = board.cpu();
	std::printf("t=%lld cycles  pc=%05x  halted=%d  status=%02x  dac_events=%zu\n",
		(long long)board.now_cycles(), cpu.phys_pc(), cpu.halted() ? 1 : 0,
		board.host_status(), board.dac_event_count());
	std::printf("  AX=%04x CX=%04x DX=%04x BX=%04x SP=%04x BP=%04x SI=%04x DI=%04x\n",
		cpu.wreg(0), cpu.wreg(1), cpu.wreg(2), cpu.wreg(3),
		cpu.wreg(4), cpu.wreg(5), cpu.wreg(6), cpu.wreg(7));
	std::printf("  ES=%04x CS=%04x SS=%04x DS=%04x IP=%04x\n",
		cpu.sreg(0), cpu.sreg(1), cpu.sreg(2), cpu.sreg(3), cpu.reg_ip());
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s <rom.bin> trace|boot|say ...\n", argv[0]);
		return 2;
	}

	std::vector<u8> rom = read_file(argv[1]);
	doubletalk_board board;
	if (!board.load_rom(rom.data(), rom.size()))
	{
		std::fprintf(stderr, "bad ROM size %zu (want %u)\n", rom.size(), doubletalk_board::ROM_SIZE);
		return 2;
	}
	board.reset();

	std::string cmd = argv[2];

	if (cmd == "trace")
	{
		long max_instr = (argc > 3) ? std::atol(argv[3]) : 200;
		long count = 0;
		board.cpu().trace_hook = [&](offs_t pc)
		{
			doubletalk_cpu &c = board.cpu();
			if (count < max_instr)
				std::printf("%6ld %05x AX=%04x CX=%04x DX=%04x BX=%04x SP=%04x SI=%04x DI=%04x ES=%04x DS=%04x SS=%04x\n",
					count, pc, c.wreg(0), c.wreg(1), c.wreg(2), c.wreg(3), c.wreg(4),
					c.wreg(6), c.wreg(7), c.sreg(0), c.sreg(3), c.sreg(2));
			count++;
		};
		// enough cycles to cover boot; stop early at HLT
		for (int i = 0; i < 20000 && !board.cpu().halted() && count < max_instr; i++)
			board.run_cycles(1000);
		board.cpu().trace_hook = nullptr;
		print_state(board);
		return 0;
	}

	if (cmd == "boot")
	{
		s64 cycles = (argc > 3) ? std::atoll(argv[3]) : 20000000; // 2s default
		board.run_cycles(cycles);
		print_state(board);
		return 0;
	}

	if (cmd == "say")
	{
		if (argc < 5)
		{
			std::fprintf(stderr, "usage: %s <rom.bin> say <text> <out.wav>\n", argv[0]);
			return 2;
		}
		std::string text = argv[3];
		std::string out_path = argv[4];

		// Firmware programs timer0 for a 952-cycle period (measured; matches
		// the ~10.5kHz cadence documented in PORTING.md): 10MHz/952 = 10504Hz.
		const u32 rate = doubletalk_board::CPU_HZ / 952;
		std::vector<u8> pcm;

		// Boot until RDY (firmware sets the status byte itself)
		s64 boot_limit = s64(doubletalk_board::CPU_HZ) * 10;
		while (!board.rdy() && board.now_cycles() < boot_limit)
			board.run_cycles(10000);
		if (!board.rdy())
		{
			std::fprintf(stderr, "firmware never asserted RDY (status=%02x)\n", board.host_status());
			print_state(board);
			return 1;
		}
		std::fprintf(stderr, "RDY after %lld cycles (%.3fs emulated), status=%02x\n",
			(long long)board.now_cycles(),
			double(board.now_cycles()) / doubletalk_board::CPU_HZ, board.host_status());
		// discard any boot-time audio (e.g. power-on click) before speaking
		board.pull_samples(pcm, rate);
		pcm.clear();

		// send text + CR, RDY-gated like a real host driver
		std::string payload = text;
		payload.push_back('\r');
		for (char ch : payload)
		{
			s64 waited = 0;
			while (!board.rdy() && waited < s64(doubletalk_board::CPU_HZ) * 5)
				waited += board.run_cycles(1000);
			if (!board.rdy())
			{
				std::fprintf(stderr, "RDY timeout mid-send (status=%02x)\n", board.host_status());
				print_state(board);
				return 1;
			}
			board.host_write(u8(ch));
			board.run_cycles(100);
		}

		// run until DAC has been quiet for 0.5s emulated (or hard cap)
		s64 quiet_cycles = 0;
		size_t last_events = board.dac_event_count();
		s64 hard_cap = board.now_cycles() + s64(doubletalk_board::CPU_HZ) * 120;
		while (quiet_cycles < doubletalk_board::CPU_HZ / 2 && board.now_cycles() < hard_cap)
		{
			board.run_cycles(doubletalk_board::CPU_HZ / 100); // 10ms steps
			if (board.dac_event_count() != last_events)
			{
				last_events = board.dac_event_count();
				quiet_cycles = 0;
			}
			else
				quiet_cycles += doubletalk_board::CPU_HZ / 100;
		}

		board.pull_samples(pcm, rate);
		if (!write_wav_u8(out_path, pcm, rate))
		{
			std::fprintf(stderr, "failed writing %s\n", out_path.c_str());
			return 1;
		}
		std::fprintf(stderr, "wrote %zu samples (%.2fs) to %s, dac_events=%zu\n",
			pcm.size(), double(pcm.size()) / rate, out_path.c_str(), board.dac_event_count());
		print_state(board);
		return 0;
	}

	if (cmd == "cadence")
	{
		// speak a phrase but analyze DAC write intervals instead of audio
		std::string text = (argc > 3) ? argv[3] : "HELLO";
		s64 boot_limit = s64(doubletalk_board::CPU_HZ) * 10;
		while (!board.rdy() && board.now_cycles() < boot_limit)
			board.run_cycles(10000);
		std::string payload = text;
		payload.push_back('\r');
		for (char ch : payload)
		{
			while (!board.rdy())
				board.run_cycles(1000);
			board.host_write(u8(ch));
			board.run_cycles(100);
		}
		board.run_cycles(s64(doubletalk_board::CPU_HZ) * 5);

		const auto &ev = board.dac_events();
		std::map<s64, long> hist;
		for (size_t i = 1; i < ev.size(); i++)
			hist[ev[i].first - ev[i - 1].first]++;
		std::printf("%zu events, inter-write cycle deltas:\n", ev.size());
		for (auto &kv : hist)
			std::printf("  delta %6lld cycles (%.1f Hz): %ld times\n",
				(long long)kv.first, double(doubletalk_board::CPU_HZ) / kv.first, kv.second);
		return 0;
	}

	std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
	return 2;
}
