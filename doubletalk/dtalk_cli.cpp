// license:BSD-3-Clause
// copyright-holders:David Sexton
// CLI harness for the standalone DoubleTalk PC board.
//
//   dtalk_cli <rom> trace [max_instructions]     - boot trace to stdout
//   dtalk_cli <rom> boot [cycles]                - run boot, report state
//   dtalk_cli <rom> say <text> <out.wav>         - synthesize to 8-bit WAV
//   dtalk_cli <rom> say16 <text> <out.wav> [lowpass_hz]
//                                                - synthesize to 16-bit WAV
//                                                  (modeled output stage);
//                                                  lowpass_hz sets the
//                                                  reconstruction low-pass
//                                                  corner (default 3000)
//   dtalk_cli <rom> say16boost <level> <text> <out.wav> [lowpass_hz]
//                                                - say16 with a rate-boost
//                                                  level (dtalk_set_rate_boost)
//   dtalk_cli <rom> booststress [level]          - multi-utterance safety run
//                                                  at a rate-boost level
//   dtalk_cli <rom> dacprobe <level|pct> <text>  - DIAGNOSTIC: speak at a boost
//                                                  level (0/1/2) or raw uniform
//                                                  period percent (>=50) and
//                                                  report raw DAC-event ground
//                                                  truth: card_busy-vs-last-
//                                                  event timing, tail envelope,
//                                                  and period-table index usage

#include "doubletalk_board.h"
#include "dtalk.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <map>
#include <utility>
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

static bool write_wav_s16(const std::string &path, const std::vector<int16_t> &pcm, u32 rate)
{
	std::ofstream f(path, std::ios::binary);
	if (!f)
		return false;
	u32 data_size = u32(pcm.size() * 2);
	u32 byte_rate = rate * 2;
	u8 hdr[44] = {
		'R','I','F','F', 0,0,0,0, 'W','A','V','E',
		'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
		0,0,0,0, 0,0,0,0, 2,0, 16,0,
		'd','a','t','a', 0,0,0,0
	};
	u32 riff_size = 36 + data_size;
	std::memcpy(hdr + 4, &riff_size, 4);
	std::memcpy(hdr + 24, &rate, 4);
	std::memcpy(hdr + 28, &byte_rate, 4);
	std::memcpy(hdr + 40, &data_size, 4);
	f.write(reinterpret_cast<char *>(hdr), sizeof(hdr));
	f.write(reinterpret_cast<const char *>(pcm.data()), data_size);
	return bool(f);
}

static void print_state(doubletalk_board &board)
{
	doubletalk_cpu &cpu = board.cpu();
	std::printf("t=%lld cycles  pc=%05x  halted=%d  status=%02x  lpc=%02x  dac_events=%zu\n",
		(long long)board.now_cycles(), cpu.phys_pc(), cpu.halted() ? 1 : 0,
		board.host_status(), board.host_lpc_status(), board.dac_event_count());
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
		// exercised through the public C API (dtalk.h) end to end
		if (argc < 5)
		{
			std::fprintf(stderr, "usage: %s <rom.bin> say <text> <out.wav>\n", argv[0]);
			return 2;
		}
		std::string text = argv[3];
		std::string out_path = argv[4];

		dtalk *dt = dtalk_create(rom.data(), rom.size());
		if (!dt)
		{
			std::fprintf(stderr, "dtalk_create failed\n");
			return 1;
		}
		dtalk_say(dt, text.c_str());

		std::vector<u8> pcm;
		u8 buf[4096];
		size_t n;
		while ((n = dtalk_synth(dt, buf, sizeof(buf))) > 0)
			pcm.insert(pcm.end(), buf, buf + n);

		dtalk_index_mark mk[64];
		size_t nmk = dtalk_read_index_marks(dt, mk, 64);
		for (size_t i = 0; i < nmk; i++)
			std::fprintf(stderr, "index mark %u at sample %llu (%.3fs)\n",
				mk[i].value, (unsigned long long)mk[i].sample_pos,
				double(mk[i].sample_pos) / dtalk_sample_rate(dt));

		u32 rate = dtalk_sample_rate(dt);
		dtalk_destroy(dt);

		if (!write_wav_u8(out_path, pcm, rate))
		{
			std::fprintf(stderr, "failed writing %s\n", out_path.c_str());
			return 1;
		}
		std::fprintf(stderr, "wrote %zu samples (%.2fs at %uHz) to %s\n",
			pcm.size(), double(pcm.size()) / rate, rate, out_path.c_str());
		return 0;
	}

	if (cmd == "say16")
	{
		// Same as say, but through the modeled 16-bit output stage.
		if (argc < 5)
		{
			std::fprintf(stderr, "usage: %s <rom.bin> say16 <text> <out.wav> [lowpass_hz]\n", argv[0]);
			return 2;
		}
		std::string text = argv[3];
		std::string out_path = argv[4];
		// Optional reconstruction low-pass corner (default 3000 = authentic).
		u32 lowpass_hz = (argc > 5) ? u32(std::atol(argv[5])) : 0;

		dtalk *dt = dtalk_create(rom.data(), rom.size());
		if (!dt)
		{
			std::fprintf(stderr, "dtalk_create failed\n");
			return 1;
		}
		if (lowpass_hz)
			dtalk_set_lowpass_hz(dt, lowpass_hz);
		dtalk_say(dt, text.c_str());

		std::vector<int16_t> pcm;
		int16_t buf[4096];
		size_t n;
		while ((n = dtalk_synth16(dt, buf, 4096)) > 0)
			pcm.insert(pcm.end(), buf, buf + n);

		u32 rate = dtalk_sample_rate(dt);
		dtalk_destroy(dt);

		if (!write_wav_s16(out_path, pcm, rate))
		{
			std::fprintf(stderr, "failed writing %s\n", out_path.c_str());
			return 1;
		}
		std::fprintf(stderr, "wrote %zu samples (%.2fs at %uHz, 16-bit) to %s\n",
			pcm.size(), double(pcm.size()) / rate, rate, out_path.c_str());
		return 0;
	}

	if (cmd == "booststress")
	{
		// SAFETY: one session at max boost - speak several different phrases,
		// interleave a mid-utterance stop, and confirm the card keeps
		// producing audio, returns to idle (RDY) between utterances, and never
		// hangs or faults.
		int level = (argc > 3) ? std::atoi(argv[3]) : dtalk_rate_boost_max();
		dtalk *dt = dtalk_create(rom.data(), rom.size());
		if (!dt) { std::fprintf(stderr, "dtalk_create failed\n"); return 1; }
		dtalk_set_rate_boost(dt, level);
		const char *phrases[] = {
			"\x019S THE FIVE BOXING WIZARDS JUMP QUICKLY",
			"\x019S PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS",
			"\x019S HOW VEXINGLY QUICK DAFT ZEBRAS JUMP",
			"\x019S SPHINX OF BLACK QUARTZ JUDGE MY VOW",
			"\x019S THE QUICK ONYX GOBLIN JUMPS OVER THE LAZY DWARF",
		};
		int rc = 0;
		u8 buf[4096];
		for (int i = 0; i < 5; i++)
		{
			dtalk_say(dt, phrases[i]);
			size_t got = 0, n;
			while ((n = dtalk_synth(dt, buf, sizeof(buf))) > 0)
				got += n;
			int idle = (dtalk_active(dt) == 0);
			std::printf("phrase %d: %zu samples (%.2fs), idle_after=%d, boost=%d\n",
				i, got, double(got) / dtalk_sample_rate(dt), idle,
				dtalk_get_rate_boost(dt));
			if (got == 0) { std::printf("  NO AUDIO - FAIL\n"); rc = 1; }
			if (!idle) { std::printf("  card did not return to idle - FAIL\n"); rc = 1; }
			if (i == 2) // cancel mid-stream on the next one to stress the path
			{
				dtalk_say(dt, "\x019S THIS UTTERANCE IS CANCELLED PARTWAY");
				dtalk_synth(dt, buf, sizeof(buf));
				dtalk_stop(dt);
				std::printf("  (mid-utterance stop OK, active=%d)\n", dtalk_active(dt));
			}
		}
		std::printf("%s\n", rc ? "STRESS FAIL" : "STRESS PASS");
		dtalk_destroy(dt);
		return rc;
	}

	if (cmd == "say16boost")
	{
		// say16 with a rate-boost level applied (dtalk_set_rate_boost).
		//   dtalk_cli <rom> say16boost <level> <text> <out.wav> [lowpass_hz]
		if (argc < 6)
		{
			std::fprintf(stderr, "usage: %s <rom> say16boost <level> <text> <out.wav> [lowpass_hz]\n", argv[0]);
			return 2;
		}
		int level = std::atoi(argv[3]);
		std::string text = argv[4];
		std::string out_path = argv[5];
		u32 lowpass_hz = (argc > 6) ? u32(std::atol(argv[6])) : 0;

		dtalk *dt = dtalk_create(rom.data(), rom.size());
		if (!dt) { std::fprintf(stderr, "dtalk_create failed\n"); return 1; }
		dtalk_set_rate_boost(dt, level);
		if (lowpass_hz)
			dtalk_set_lowpass_hz(dt, lowpass_hz);
		dtalk_say(dt, text.c_str());

		std::vector<int16_t> pcm;
		int16_t buf[4096];
		size_t n;
		while ((n = dtalk_synth16(dt, buf, 4096)) > 0)
			pcm.insert(pcm.end(), buf, buf + n);
		u32 rate = dtalk_sample_rate(dt);
		dtalk_destroy(dt);

		if (!write_wav_s16(out_path, pcm, rate))
		{
			std::fprintf(stderr, "failed writing %s\n", out_path.c_str());
			return 1;
		}
		std::fprintf(stderr, "boost=%d wrote %zu samples (%.2fs at %uHz, 16-bit) to %s\n",
			level, pcm.size(), double(pcm.size()) / rate, rate, out_path.c_str());
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

	if (cmd == "stoptest")
	{
		// Regression for the index-mark drift after a cancel: queue an
		// utterance with a marker, run partway, dtalk_stop (discards audio
		// that still advanced the board's absolute sample grid), then speak a
		// second utterance with a marker and confirm the marker's reported
		// sample_pos lands within the delivered stream, not beyond its end.
		dtalk *dt = dtalk_create(rom.data(), rom.size());
		if (!dt)
		{
			std::fprintf(stderr, "dtalk_create failed\n");
			return 1;
		}
		u8 buf[4096];

		// Simulate a screen-reader session: many utterances each cancelled
		// mid-synthesis. Every cancel discards a little pending audio that
		// already advanced the board's absolute sample grid, so the drift
		// between grid and delivered stream accumulates across the session.
		const char u1[] = "\x01" "0I" " ONE TWO THREE FOUR FIVE SIX SEVEN EIGHT NINE\r";
		const int CANCELS = 30;
		size_t drained1 = 0;
		dtalk_index_mark scrap[64];
		for (int c = 0; c < CANCELS; c++)
		{
			dtalk_queue(dt, u1, sizeof(u1) - 1);
			drained1 += dtalk_synth(dt, buf, sizeof(buf)); // one chunk, then cancel
			dtalk_stop(dt);
			dtalk_read_index_marks(dt, scrap, 64); // stop already cleared these
		}
		std::fprintf(stderr, "%d cancelled utterances: delivered %zu samples total\n",
			CANCELS, drained1);

		// Utterance 2: marker 1 a few words in. Its mark position WITHIN this
		// utterance must not depend on the preceding cancels.
		const char u2[] = "EIGHT NINE TEN \x01" "1I" " ELEVEN TWELVE\r";
		u32 rate = dtalk_sample_rate(dt);

		auto speak_u2 = [&](dtalk *d) -> std::pair<size_t, long long>
		{
			dtalk_queue(d, u2, sizeof(u2) - 1);
			size_t got = 0, n;
			while ((n = dtalk_synth(d, buf, sizeof(buf))) > 0)
				got += n;
			dtalk_index_mark mk[64];
			size_t nmk = dtalk_read_index_marks(d, mk, 64);
			return { got, nmk ? (long long)mk[0].sample_pos : -1 };
		};

		auto [delivered, markPos] = speak_u2(dt);
		std::fprintf(stderr, "after %d cancels: u2 delivered %zu samples, "
			"mark 1 at cumulative sample %lld (%.3fs)\n",
			CANCELS, delivered, markPos, markPos / double(rate));

		// Baseline: same utterance on a fresh instance, no cancels.
		dtalk *base = dtalk_create(rom.data(), rom.size());
		auto [baseDelivered, baseMark] = speak_u2(base);
		dtalk_destroy(base);
		std::fprintf(stderr, "baseline (no cancels): u2 delivered %zu samples, "
			"mark 1 at sample %lld (%.3fs)\n",
			baseDelivered, baseMark, baseMark / double(rate));

		int rc = 0;
		if (markPos < 0 || baseMark < 0)
		{
			std::fprintf(stderr, "  no marks reported!\n");
			rc = 1;
		}
		else
		{
			// mark position relative to u2's own start must match the baseline.
			long long relPos = markPos - (long long)drained1;
			long long drift = relPos - baseMark;
			std::fprintf(stderr, "  mark offset into u2: %lld vs baseline %lld -> "
				"drift %lld samples (%.3fs)\n", relPos, baseMark, drift,
				drift / double(rate));
			if (llabs(drift) > 32) // a couple ms of grid-rounding slack
			{
				std::fprintf(stderr, "  MARK DRIFT - BUG\n");
				rc = 1;
			}
			else
				std::fprintf(stderr, "  mark aligned with delivered stream\n");
		}
		dtalk_destroy(dt);
		return rc;
	}

	if (cmd == "ratewatch")
	{
		// DEBUG: watch reads/writes of a RAM byte while speaking, reporting
		// each accessing instruction's PC with a hit count - handy for finding
		// the firmware code that produces/consumes a given variable in the
		// disassembly.  usage: ratewatch [addr] [text]
		u32 watch = (argc > 3) ? u32(std::strtoul(argv[3], nullptr, 0)) : 0x00a2;
		std::string text = (argc > 4) ? argv[4] : "\x01" "9S HELLO WORLD";
		std::map<std::pair<u32, bool>, long> hits; // (pc,is_write) -> count
		board.cpu().trace_hook = nullptr;
		u32 last_pc = 0;
		board.cpu().trace_hook = [&](offs_t pc) { last_pc = board.cpu().phys_pc(); };
		board.ram_access_hook = [&](offs_t addr, bool is_write, u8 data)
		{
			if (addr == watch)
				hits[{ last_pc, is_write }]++;
		};
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
			board.run_cycles(2000);
		}
		board.run_cycles(s64(doubletalk_board::CPU_HZ) * 3);
		board.ram_access_hook = nullptr;
		board.cpu().trace_hook = nullptr;
		std::printf("accesses to %04x:\n", watch);
		for (auto &kv : hits)
			std::printf("  pc=%05x %s  x%ld\n", kv.first.first,
				kv.first.second ? "WRITE" : "READ ", kv.second);
		return 0;
	}

	if (cmd == "dacprobe")
	{
		// DIAGNOSTIC: speak a word at a rate-boost level at the board level and
		// report, in raw DAC-event ground truth: when card_busy() first goes
		// false, and when the LAST DAC event actually fires. If the last DAC
		// event is well after card_busy went false, the firmware is still
		// draining audio after it looks idle (capture-window relevant).
		//   dtalk_cli <rom> dacprobe <level> <text>
		int arg = (argc > 3) ? std::atoi(argv[3]) : 0;
		std::string text = (argc > 4) ? argv[4] : "CONFIGURATION";

		// arg 0/1/2 = boost level (PCT table); arg >= 50 = raw uniform percent.
		const u32 RATE_TABLE_OFF = 0x48da;
		const int RATE_REGION_LO = 10, RATE_REGION_HI = 22;
		const int PCT[] = { 100, 88, 80 };
		int pct = (arg >= 50) ? arg : PCT[arg < 0 ? 0 : (arg > 2 ? 2 : arg)];
		int level = arg;
		// Mirror dtalk's apply_rate_boost, including the stock-minimum period
		// floor that keeps final frames out of the firmware's degenerate path.
		u32 stock_min = 0xffff;
		for (int i = RATE_REGION_LO; i <= RATE_REGION_HI; i++)
		{
			u32 off = RATE_TABLE_OFF + 2u * u32(i);
			u32 p = u32(board.rom_peek(off)) | u32(board.rom_peek(off + 1)) << 8;
			if (p < stock_min) stock_min = p;
		}
		for (int i = RATE_REGION_LO; i <= RATE_REGION_HI; i++)
		{
			u32 off = RATE_TABLE_OFF + 2u * u32(i);
			u32 w = (u32(board.rom_peek(off)) | u32(board.rom_peek(off + 1)) << 8);
			w = w * u32(pct) / 100u;
			if (w < stock_min) w = stock_min;
			if (w > 0xffff) w = 0xffff;
			board.rom_poke(off, u8(w & 0xff));
			board.rom_poke(off + 1, u8((w >> 8) & 0xff));
		}

		const u32 BUF_READ_PTR = 0x000f, BUF_WRITE_PTR = 0x0011;
		auto card_busy = [&]() {
			return (board.host_status() & 0x40) != 0
				|| board.ram16(BUF_READ_PTR) != board.ram16(BUF_WRITE_PTR);
		};

		// Track which period-table index the firmware reads at 0x84b57
		// (mov cs:0x48da(di),ax): di = index*2. Record (cycle, index).
		std::vector<std::pair<s64,int>> idx_hits;
		board.cpu().trace_hook = [&](offs_t) {
			if (board.cpu().phys_pc() == 0x84b57)
				idx_hits.emplace_back(board.now_cycles(), int(board.cpu().wreg(7)) / 2);
		};

		s64 boot_limit = s64(doubletalk_board::CPU_HZ) * 10;
		while (!board.rdy() && board.now_cycles() < boot_limit)
			board.run_cycles(10000);
		// swallow power-on click window
		board.run_cycles(doubletalk_board::CPU_HZ / 10);
		size_t dac_before = board.dac_events().size();

		std::string payload = text; payload.push_back('\r');
		for (char ch : payload)
		{
			while (!board.rdy())
				board.run_cycles(1000);
			board.host_write(u8(ch));
			board.run_cycles(2000);
		}

		// Run to fully idle, watching card_busy transitions and DAC events.
		const s64 CHUNK = doubletalk_board::CPU_HZ / 1000; // 1ms
		s64 busy_false_cycle = -1;
		bool was_busy = true;
		s64 quiet_needed = s64(doubletalk_board::CPU_HZ) * 2; // 2s hard stop
		s64 start = board.now_cycles();
		s64 last_seen_event_cycle = start;
		size_t last_count = board.dac_events().size();
		while (board.now_cycles() - start < quiet_needed)
		{
			board.run_cycles(CHUNK);
			const auto &ev = board.dac_events();
			if (ev.size() != last_count)
			{
				last_seen_event_cycle = ev.back().first;
				last_count = ev.size();
			}
			bool busy = card_busy();
			if (was_busy && !busy)
				busy_false_cycle = board.now_cycles();
			if (busy)
				busy_false_cycle = -1; // reset on any re-busy
			was_busy = busy;
			// stop once quiet for 300ms and not busy
			if (!busy && board.now_cycles() - last_seen_event_cycle > doubletalk_board::CPU_HZ * 3 / 10)
				break;
		}

		const auto &ev = board.dac_events();
		size_t total = ev.size() - dac_before;
		double hz = double(doubletalk_board::CPU_HZ);
		std::printf("arg=%d pct=%d text=\"%s\"\n", level, pct, text.c_str());
		std::printf("  total DAC events: %zu\n", total);
		std::printf("  first event: %.1fms  last event: %.1fms  span: %.1fms\n",
			(ev[dac_before].first - start) / hz * 1000.0,
			(last_seen_event_cycle - start) / hz * 1000.0,
			(last_seen_event_cycle - ev[dac_before].first) / hz * 1000.0);
		if (busy_false_cycle >= 0)
			std::printf("  card_busy went false at: %.1fms\n",
				(busy_false_cycle - start) / hz * 1000.0);
		else
			std::printf("  card_busy still true at loop end\n");
		if (busy_false_cycle >= 0)
			std::printf("  DAC events AFTER card_busy false: last event is %.1fms past busy-false\n",
				(last_seen_event_cycle - busy_false_cycle) / hz * 1000.0);
		// Tail envelope: RMS of raw DAC (centered at 128) in 10ms bins, last 250ms.
		s64 binc = doubletalk_board::CPU_HZ / 100; // 10ms
		s64 tail_start = last_seen_event_cycle - doubletalk_board::CPU_HZ * 25 / 100;
		std::printf("  tail DAC envelope (10ms bins, |val-128| mean), last 250ms:\n   ");
		for (s64 t = tail_start; t < last_seen_event_cycle; t += binc)
		{
			double acc = 0; int cnt = 0;
			for (size_t k = dac_before; k < ev.size(); k++)
				if (ev[k].first >= t && ev[k].first < t + binc)
				{ acc += std::abs(int(ev[k].second) - 128); cnt++; }
			std::printf("%5.1f ", cnt ? acc / cnt : 0.0);
		}
		std::printf("\n");
		board.cpu().trace_hook = nullptr;
		// Period-table index usage: overall histogram, and the indices read in
		// the final 120ms of DAC activity (the final-consonant frames).
		{
			int hist[32] = {0};
			for (auto &h : idx_hits) if (h.second >= 0 && h.second < 32) hist[h.second]++;
			std::printf("  index histogram (idx:count):");
			for (int i = 0; i < 32; i++) if (hist[i]) std::printf(" %d:%d", i, hist[i]);
			std::printf("\n");
			s64 fin = last_seen_event_cycle - doubletalk_board::CPU_HZ * 12 / 100; // 120ms
			std::printf("  indices read in final 120ms (idx@ms scaled-period):");
			for (auto &h : idx_hits) if (h.first >= fin)
			{
				u32 off = RATE_TABLE_OFF + 2u * u32(h.second);
				u32 per = u32(board.rom_peek(off)) | u32(board.rom_peek(off + 1)) << 8;
				std::printf(" %d@%.0f(p=%u)", h.second,
					(h.first - start) / hz * 1000.0, per);
			}
			std::printf("\n");
		}
		return 0;
	}

	std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
	return 2;
}
