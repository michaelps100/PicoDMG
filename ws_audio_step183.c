Created At: 2026-10-01T15:39:23-03:00
Completed At: 2026-10-01T15:39:23-03:00
File Path: `file:///c:/dev/PicoDMG-Temp/PicoWS/src/oswan/ws_audio.c`
Total Lines: 391
Total Bytes: 14460
Showing lines 1 to 391
The following code has been modified to include a line number before every line, in the format: <line_number>: <original_line>. Please note that any changes targeting the original code should remove the line number, colon, and leading space.
1: /**
2:  * ws_audio.c - WonderSwan Sound Emulation for PicoWS
3:  *
4:  * Implements 4-channel audio generation for Bandai WonderSwan / WonderSwan Color:
5:  *   - Channel 0: 32-sample wavetable (4-bit samples)
6:  *   - Channel 1: 32-sample wavetable OR Voice / Direct PCM
7:  *   - Channel 2: 32-sample wavetable with hardware Frequency Sweep
8:  *   - Channel 3: 32-sample wavetable OR 15-bit LFSR Noise
9:  *   - Output mixed at 44.1 kHz stereo to MAX98357A I2S
10:  */
11: 
12: #include "ws_audio.h"
13: #include <string.h>
14: 
15: #include "types.h"
16: #include "memory.h"
17: #include "io.h"
18: #include "external_audio.h"
19: 
20: extern uint8_t cpu_readmem20(uint32_t addr);
21: 
22: #define WS_CLOCK_HZ            3072000ULL
23: #define AUDIO_SAMPLE_RATE       44100ULL
24: #define SAMPLES_PER_FRAME       735        /* 44100 Hz / 60 FPS */
25: #define SCANLINES_PER_FRAME     159
26: 
27: /* Cycles of 3.072MHz per 44.1kHz audio sample in 16.16 fixed-point */
28: #define CYCLES_PER_SAMPLE_FP16  ((uint32_t)((WS_CLOCK_HZ << 16) / AUDIO_SAMPLE_RATE))
29: 
30: /* Register state */
31: static uint16_t s_period[4];
32: static uint8_t  s_volume[4];
33: static uint8_t  s_control;
34: static uint8_t  s_output_control;
35: static uint8_t  s_sample_ram_pos;
36: static uint8_t  s_voice_volume;
37: static uint8_t  s_sweep_value;
38: static uint8_t  s_sweep_step;
39: static uint8_t  s_noise_control;
40: static uint16_t s_nreg = 0;
41: 
42: static int32_t  s_period_counter[4];
43: static uint8_t  s_sample_pos[4];
44: static int32_t  s_sweep_divider;
45: static int32_t  s_sweep_counter;

<truncated 12742 bytes>
le = (s_sample_pos[3] & 1) ? ((b >> 4) & 0x0F) : (b & 0x0F);
339:                 sample -= 8;
340:                 ch_l[3] = sample * ((s_volume[3] >> 4) & 0x0F);
341:                 ch_r[3] = sample * (s_volume[3] & 0x0F);
342:             }
343:         }
344:     }
345: 
346:     int32_t mix_l = ch_l[0] + ch_l[1] + ch_l[2] + ch_l[3];
347:     int32_t mix_r = ch_r[0] + ch_r[1] + ch_r[2] + ch_r[3];
348: 
349:     if (s_hvoice_ctrl & 0x80) {
350:         int32_t hv_sample = (int32_t)s_hyper_voice - 128;
351:         mix_l += hv_sample;
352:         mix_r += hv_sample;
353:     }
354: 
355:     /* Master volume scaling (Clean Light UI level 1..5) */
356:     static const int32_t vol_table[6] = { 0, 51, 102, 154, 205, 256 };
357:     int32_t cur_vol = vol_table[s_cfg_volume];
358: 
359:     int32_t out_l = (mix_l * 54 * cur_vol) >> 8;
360:     int32_t out_r = (mix_r * 54 * cur_vol) >> 8;
361: 
362:     if (out_l > 32767)  out_l = 32767;
363:     else if (out_l < -32768) out_l = -32768;
364:     if (out_r > 32767)  out_r = 32767;
365:     else if (out_r < -32768) out_r = -32768;
366: 
367:     return ((uint32_t)(uint16_t)(int16_t)out_l << 16) | (uint16_t)(int16_t)out_r;
368: }
369: 
370: /* -------------------------------------------------------------------------- */
371: void ws_audio_process_line(int line) {
372:     if (line < 0 || line >= SCANLINES_PER_FRAME) return;
373:     if (line == 0) {
374:         s_samples_rendered_in_frame = 0;
375:     }
376: 
377:     int target = (line + 1) * SAMPLES_PER_FRAME / SCANLINES_PER_FRAME;
378:     int count = target - s_samples_rendered_in_frame;
379:     if (count <= 0) return;
380: 
381:     uint32_t line_samples[8];
382:     if (count > 8) count = 8;
383: 
384:     for (int i = 0; i < count; i++) {
385:         line_samples[i] = ws_audio_render_sample();
386:     }
387:     s_samples_rendered_in_frame += count;
388: 
389:     EXT_AUDIO_ENQUEUE_SAMPLES(line_samples, count);
390: }
391: 
The above content shows the entire, complete file contents of the requested file.
