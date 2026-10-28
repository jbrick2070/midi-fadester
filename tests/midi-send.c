/*
 * Midi Fadester - WinMM CC Sweep Sender & Self-Test
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(cond)                                                               \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

static uint32_t mfd_pack_cc_short_msg(int channel_1_16, int cc_0_127, int val_0_127)
{
	uint32_t status = 0xB0U | ((uint32_t)(channel_1_16 - 1) & 0x0FU);
	uint32_t d1 = (uint32_t)cc_0_127 & 0x7FU;
	uint32_t d2 = (uint32_t)val_0_127 & 0x7FU;
	return status | (d1 << 8) | (d2 << 16);
}

static int mfd_compute_sweep_step_value(int start_val, int end_val, int step_index, int total_intervals)
{
	if (total_intervals <= 0 || step_index <= 0) {
		return start_val;
	}
	if (step_index >= total_intervals) {
		return end_val;
	}
	int delta = end_val - start_val;
	return start_val + (delta * step_index) / total_intervals;
}

static int mfd_compute_slow_pause_wait_ms(int val, int end_val, int step_ms, int pause_cc, int pause_ms)
{
	if (val == end_val) {
		return 0;
	}
	int wait_ms = (step_ms > 0) ? step_ms : 0;
	if (val == pause_cc && pause_ms > 0) {
		wait_ms += pause_ms;
	}
	return wait_ms;
}

static bool match_port_substring_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || needle[0] == '\0') {
		return false;
	}
	size_t nlen = strlen(needle);
	for (const char *p = haystack; *p != '\0'; ++p) {
		if (_strnicmp(p, needle, nlen) == 0) {
			return true;
		}
	}
	return false;
}

static int run_self_test(void)
{
	/* 1. Verify WinMM short message packing for Ch 1 CC 15 = 64 */
	uint32_t msg1 = mfd_pack_cc_short_msg(1, 15, 64);
	ASSERT_TRUE((msg1 & 0xFFU) == 0xB0U);
	ASSERT_TRUE(((msg1 >> 8) & 0x7FU) == 15U);
	ASSERT_TRUE(((msg1 >> 16) & 0x7FU) == 64U);

	/* 2. Verify Ch 16 CC 7 = 127 */
	uint32_t msg16 = mfd_pack_cc_short_msg(16, 7, 127);
	ASSERT_TRUE((msg16 & 0xFFU) == 0xBFU);
	ASSERT_TRUE(((msg16 >> 8) & 0x7FU) == 7U);
	ASSERT_TRUE(((msg16 >> 16) & 0x7FU) == 127U);

	/* 3. Verify 0..127 sweep schedule endpoints and midpoint */
	ASSERT_TRUE(mfd_compute_sweep_step_value(0, 127, 0, 127) == 0);
	ASSERT_TRUE(mfd_compute_sweep_step_value(0, 127, 64, 127) == 64);
	ASSERT_TRUE(mfd_compute_sweep_step_value(0, 127, 127, 127) == 127);

	/* 4. Verify reverse sweep 127..0 */
	ASSERT_TRUE(mfd_compute_sweep_step_value(127, 0, 0, 127) == 127);
	ASSERT_TRUE(mfd_compute_sweep_step_value(127, 0, 127, 127) == 0);

	/* 5. Verify case-insensitive port matching */
	ASSERT_TRUE(match_port_substring_ci("loopMIDI Port 1", "loopMIDI"));
	ASSERT_TRUE(match_port_substring_ci("loopMIDI Port", "loopmidi"));
	ASSERT_TRUE(!match_port_substring_ci("Microsoft GS Wavetable Synth", "loopMIDI"));

	/* 6. Verify --slow-pause step & pause wait schedule (100..127 @ 300 ms/CC + 2000 ms pause at CC 116) */
	ASSERT_TRUE(mfd_compute_slow_pause_wait_ms(100, 127, 300, 116, 2000) == 300);
	ASSERT_TRUE(mfd_compute_slow_pause_wait_ms(116, 127, 300, 116, 2000) == 2300);
	ASSERT_TRUE(mfd_compute_slow_pause_wait_ms(119, 127, 300, 116, 2000) == 300);
	ASSERT_TRUE(mfd_compute_slow_pause_wait_ms(127, 127, 300, 116, 2000) == 0);

	printf("test-midi-send: PASS\n");
	return 0;
}

static int find_midi_out_port(const char *port_substring, char *out_name, size_t out_name_size)
{
	UINT num_devs = midiOutGetNumDevs();
	for (UINT i = 0; i < num_devs; ++i) {
		MIDIOUTCAPSA caps;
		memset(&caps, 0, sizeof(caps));
		if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
			if (match_port_substring_ci(caps.szPname, port_substring)) {
				if (out_name && out_name_size > 0U) {
					snprintf(out_name, out_name_size, "%s", caps.szPname);
				}
				return (int)i;
			}
		}
	}
	return -1;
}

static void send_cc_sweep_on_handle(HMIDIOUT hout, int channel, int cc, int duration_ms, int start_val, int end_val)
{
	int total_intervals = abs(end_val - start_val);
	LARGE_INTEGER freq;
	LARGE_INTEGER t0;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);

	for (int step = 0; step <= total_intervals; ++step) {
		int val = mfd_compute_sweep_step_value(start_val, end_val, step, total_intervals);
		uint32_t msg = mfd_pack_cc_short_msg(channel, cc, val);
		midiOutShortMsg(hout, (DWORD)msg);

		if (step < total_intervals && duration_ms > 0 && total_intervals > 0) {
			double target_ms = ((double)duration_ms * (double)(step + 1)) / (double)total_intervals;
			for (;;) {
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				double elapsed_ms =
					((double)(now.QuadPart - t0.QuadPart) * 1000.0) / (double)freq.QuadPart;
				double remain_ms = target_ms - elapsed_ms;
				if (remain_ms <= 0.2) {
					break;
				}
				if (remain_ms >= 2.0) {
					Sleep(1);
				} else {
					SwitchToThread();
				}
			}
		}
	}
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "--self-test") == 0) {
		return run_self_test();
	}

	if (argc >= 2 && strcmp(argv[1], "--rapid-throws") == 0) {
		if (argc < 7) {
			fprintf(stderr,
				"Usage: %s --rapid-throws <port_name> <channel_1_16> <cc_0_127> <sweep_ms> <pause_ms>\n",
				argv[0]);
			return 2;
		}
		const char *port_arg = argv[2];
		int channel = atoi(argv[3]);
		int cc = atoi(argv[4]);
		int sweep_ms = atoi(argv[5]);
		int pause_ms = atoi(argv[6]);

		if (channel < 1 || channel > 16 || cc < 0 || cc > 127 || sweep_ms < 0 || pause_ms < 0) {
			fprintf(stderr, "midi-send --rapid-throws: invalid parameters\n");
			return 2;
		}

		char resolved_name[128] = {0};
		int dev_id = find_midi_out_port(port_arg, resolved_name, sizeof(resolved_name));
		if (dev_id < 0) {
			fprintf(stderr, "midi-send: no WinMM MIDI Out port matching '%s'\n", port_arg);
			return 1;
		}

		HMIDIOUT hout = NULL;
		MMRESULT mm = midiOutOpen(&hout, (UINT)dev_id, 0, 0, CALLBACK_NULL);
		if (mm != MMSYSERR_NOERROR || !hout) {
			fprintf(stderr, "midi-send: midiOutOpen failed for '%s' (mm=%u)\n", resolved_name,
				(unsigned)mm);
			return 1;
		}

		timeBeginPeriod(1);
		send_cc_sweep_on_handle(hout, channel, cc, sweep_ms, 0, 127);
		if (pause_ms > 0) {
			Sleep((DWORD)pause_ms);
		}
		send_cc_sweep_on_handle(hout, channel, cc, sweep_ms, 127, 0);
		if (pause_ms > 0) {
			Sleep((DWORD)pause_ms);
		}
		send_cc_sweep_on_handle(hout, channel, cc, sweep_ms, 0, 127);
		timeEndPeriod(1);
		midiOutClose(hout);

		printf("midi-send: sent 3 rapid throws (sweep=%d ms, pause=%d ms) ch=%d cc=%d to '%s'\n", sweep_ms,
		       pause_ms, channel, cc, resolved_name);
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "--slow-pause") == 0) {
		if (argc < 10) {
			fprintf(stderr,
				"Usage: %s --slow-pause <port_name> <channel_1_16> <cc_0_127> <start_0_127> <end_0_127> <step_ms> <pause_cc> <pause_ms>\n",
				argv[0]);
			return 2;
		}
		const char *port_arg = argv[2];
		int channel = atoi(argv[3]);
		int cc = atoi(argv[4]);
		int start_val = atoi(argv[5]);
		int end_val = atoi(argv[6]);
		int step_ms = atoi(argv[7]);
		int pause_cc = atoi(argv[8]);
		int pause_ms = atoi(argv[9]);

		if (channel < 1 || channel > 16 || cc < 0 || cc > 127 || start_val < 0 || start_val > 127 ||
		    end_val < 0 || end_val > 127 || step_ms < 0 || pause_cc < -1 || pause_cc > 127 || pause_ms < 0) {
			fprintf(stderr, "midi-send --slow-pause: invalid parameters\n");
			return 2;
		}

		char resolved_name[128] = {0};
		int dev_id = find_midi_out_port(port_arg, resolved_name, sizeof(resolved_name));
		if (dev_id < 0) {
			fprintf(stderr, "midi-send: no WinMM MIDI Out port matching '%s'\n", port_arg);
			return 1;
		}

		HMIDIOUT hout = NULL;
		MMRESULT mm = midiOutOpen(&hout, (UINT)dev_id, 0, 0, CALLBACK_NULL);
		if (mm != MMSYSERR_NOERROR || !hout) {
			fprintf(stderr, "midi-send: midiOutOpen failed for '%s' (mm=%u)\n", resolved_name,
				(unsigned)mm);
			return 1;
		}

		timeBeginPeriod(1);
		LARGE_INTEGER freq;
		LARGE_INTEGER t0;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);
		double target_ms = 0.0;
		int dir = (end_val >= start_val) ? 1 : -1;

		for (int val = start_val;; val += dir) {
			uint32_t msg = mfd_pack_cc_short_msg(channel, cc, val);
			midiOutShortMsg(hout, (DWORD)msg);

			int wait_ms = mfd_compute_slow_pause_wait_ms(val, end_val, step_ms, pause_cc, pause_ms);
			if (val == end_val) {
				break;
			}
			if (wait_ms > 0) {
				target_ms += (double)wait_ms;
				for (;;) {
					LARGE_INTEGER now;
					QueryPerformanceCounter(&now);
					double elapsed_ms =
						((double)(now.QuadPart - t0.QuadPart) * 1000.0) / (double)freq.QuadPart;
					double remain_ms = target_ms - elapsed_ms;
					if (remain_ms <= 0.2) {
						break;
					}
					if (remain_ms >= 2.0) {
						Sleep(1);
					} else {
						SwitchToThread();
					}
				}
			}
		}

		timeEndPeriod(1);
		midiOutClose(hout);

		printf("midi-send: sent --slow-pause ch=%d cc=%d %d..%d (step=%d ms, pause_cc=%d pause=%d ms) to '%s'\n",
		       channel, cc, start_val, end_val, step_ms, pause_cc, pause_ms, resolved_name);
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "--jitter") == 0) {
		if (argc < 9) {
			fprintf(stderr,
				"Usage: %s --jitter <port_name> <channel_1_16> <cc_0_127> <interval_ms> <steps> <val_a> <val_b>\n",
				argv[0]);
			return 2;
		}
		const char *port_arg = argv[2];
		int channel = atoi(argv[3]);
		int cc = atoi(argv[4]);
		int interval_ms = atoi(argv[5]);
		int steps = atoi(argv[6]);
		int val_a = atoi(argv[7]);
		int val_b = atoi(argv[8]);

		if (channel < 1 || channel > 16 || cc < 0 || cc > 127 || interval_ms < 0 || steps <= 0 || val_a < 0 ||
		    val_a > 127 || val_b < 0 || val_b > 127) {
			fprintf(stderr, "midi-send --jitter: invalid parameters\n");
			return 2;
		}

		char resolved_name[128] = {0};
		int dev_id = find_midi_out_port(port_arg, resolved_name, sizeof(resolved_name));
		if (dev_id < 0) {
			fprintf(stderr, "midi-send: no WinMM MIDI Out port matching '%s'\n", port_arg);
			return 1;
		}

		HMIDIOUT hout = NULL;
		MMRESULT mm = midiOutOpen(&hout, (UINT)dev_id, 0, 0, CALLBACK_NULL);
		if (mm != MMSYSERR_NOERROR || !hout) {
			fprintf(stderr, "midi-send: midiOutOpen failed for '%s' (mm=%u)\n", resolved_name,
				(unsigned)mm);
			return 1;
		}

		timeBeginPeriod(1);
		for (int i = 0; i < steps; ++i) {
			int val = ((i % 2) == 0) ? val_a : val_b;
			uint32_t msg = mfd_pack_cc_short_msg(channel, cc, val);
			midiOutShortMsg(hout, (DWORD)msg);
			if (interval_ms > 0) {
				Sleep((DWORD)interval_ms);
			}
		}
		timeEndPeriod(1);
		midiOutClose(hout);

		printf("midi-send: sent CC jitter ch=%d cc=%d (%d/%d x %d every %d ms) to '%s'\n", channel, cc, val_a,
		       val_b, steps, interval_ms, resolved_name);
		return 0;
	}

	if (argc < 5) {
		fprintf(stderr,
			"Usage: %s <port_name> <channel_1_16> <cc_0_127> <duration_ms> [start_0_127] [end_0_127]\n"
			"       %s --self-test\n",
			argv[0], argv[0]);
		return 2;
	}

	const char *port_arg = argv[1];
	int channel = atoi(argv[2]);
	int cc = atoi(argv[3]);
	int duration_ms = atoi(argv[4]);
	int start_val = (argc >= 6) ? atoi(argv[5]) : 0;
	int end_val = (argc >= 7) ? atoi(argv[6]) : 127;

	if (channel < 1 || channel > 16 || cc < 0 || cc > 127 || start_val < 0 || start_val > 127 || end_val < 0 ||
	    end_val > 127 || duration_ms < 0) {
		fprintf(stderr, "midi-send: invalid parameters (ch=%d cc=%d dur=%d range=%d..%d)\n", channel, cc,
			duration_ms, start_val, end_val);
		return 2;
	}

	char resolved_name[128] = {0};
	int dev_id = find_midi_out_port(port_arg, resolved_name, sizeof(resolved_name));
	if (dev_id < 0) {
		fprintf(stderr, "midi-send: no WinMM MIDI Out port matching '%s'\n", port_arg);
		return 1;
	}

	HMIDIOUT hout = NULL;
	MMRESULT mm = midiOutOpen(&hout, (UINT)dev_id, 0, 0, CALLBACK_NULL);
	if (mm != MMSYSERR_NOERROR || !hout) {
		fprintf(stderr, "midi-send: midiOutOpen failed for '%s' (mm=%u)\n", resolved_name, (unsigned)mm);
		return 1;
	}

	timeBeginPeriod(1);

	int total_intervals = abs(end_val - start_val);
	int total_msgs = total_intervals + 1;
	send_cc_sweep_on_handle(hout, channel, cc, duration_ms, start_val, end_val);

	timeEndPeriod(1);
	midiOutClose(hout);

	printf("midi-send: sent CC sweep ch=%d cc=%d %d..%d (%d msgs over %d ms) to '%s'\n", channel, cc, start_val,
	       end_val, total_msgs, duration_ms, resolved_name);
	return 0;
}
