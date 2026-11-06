//
// Copyright (C) 2015-2023  Markus Hiienkari <mhiienka@niksula.hut.fi>
//
// This file is part of Open Source Scan Converter project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include "system.h"
#include "video_modes.h"
#include "av_controller.h"
#include "avconfig.h"

#define VM_OUT_YMULT        (vm_conf->y_rpt+1)
#define VM_OUT_XMULT        (vm_conf->x_rpt+1)
#define VM_OUT_PCLKMULT     (((vm_conf->x_rpt+1)*(vm_conf->y_rpt+1))/(vm_conf->h_skip+1))

#include "video_modes_list.c"

const int num_video_modes_plm = sizeof(video_modes_plm_default)/sizeof(mode_data_t);

mode_data_t video_modes_plm[sizeof(video_modes_plm_default)/sizeof(mode_data_t)];

void set_default_vm_table() {
    memcpy(video_modes_plm, video_modes_plm_default, sizeof(video_modes_plm_default));
}

void vmode_hv_mult(mode_data_t *vmode, uint8_t h_mult, uint8_t v_mult) {
    uint32_t val, bp_extra;

    val = vmode->timings.h_synclen * h_mult;
    if (val > H_SYNCLEN_MAX) {
        vmode->timings.h_synclen = H_SYNCLEN_MAX;
        bp_extra = val - vmode->timings.h_synclen;
    } else {
        vmode->timings.h_synclen = val;
        bp_extra = 0;
    }

    val = (vmode->timings.h_backporch * h_mult) + bp_extra;
    if (val > H_BPORCH_MAX)
        vmode->timings.h_backporch = H_BPORCH_MAX;
    else
        vmode->timings.h_backporch = val;

    val = vmode->timings.h_active * h_mult;
    if (val > H_ACTIVE_MAX)
        vmode->timings.h_active = H_ACTIVE_MAX;
    else
        vmode->timings.h_active = val;

    vmode->timings.h_total = h_mult * vmode->timings.h_total + ((h_mult * vmode->timings.h_total_adj * 5 + 50) / 100);

    val = vmode->timings.v_synclen * v_mult;
    if (val > V_SYNCLEN_MAX) {
        vmode->timings.v_synclen = V_SYNCLEN_MAX;
        bp_extra = val - vmode->timings.v_synclen;
    } else {
        vmode->timings.v_synclen = val;
        bp_extra = 0;
    }

    val = (vmode->timings.v_backporch * v_mult) + bp_extra;
    if (val > V_BPORCH_MAX)
        vmode->timings.v_backporch = V_BPORCH_MAX;
    else
        vmode->timings.v_backporch = val;

    val = vmode->timings.v_active * v_mult;
    if (val > V_ACTIVE_MAX)
        vmode->timings.v_active = V_ACTIVE_MAX;
    else
        vmode->timings.v_active = val;

    if (vmode->timings.interlaced && ((v_mult % 2) == 0)) {
        vmode->timings.interlaced = 0;
        vmode->timings.v_total *= (v_mult / 2);
    } else {
        vmode->timings.v_total *= v_mult;
    }
}

uint32_t estimate_dotclk(mode_data_t *vm_in, uint32_t h_hz) {
    if ((vm_in->type & VIDEO_SDTV) ||
        (vm_in->type & VIDEO_EDTV))
    {
        return h_hz * 858;
    } else {
        return vm_in->timings.h_total * h_hz;
    }
}

uint32_t calculate_pclk(uint32_t src_clk_hz, mode_data_t *vm_out, vm_proc_config_t *vm_conf) {
    uint32_t pclk_hz;

    if (vm_conf->si_pclk_mult > 0) {
        pclk_hz = vm_conf->si_pclk_mult*src_clk_hz;
    } else if (vm_conf->si_pclk_mult < 0) {
        pclk_hz = src_clk_hz/((-1)*vm_conf->si_pclk_mult+1);
    } else {
        // Round to kHz but maximize accuracy without using 64-bit division
        pclk_hz = (((uint32_t)((((uint64_t)vm_out->timings.h_total*vm_out->timings.v_total*vm_out->timings.v_hz_x100)>>vm_out->timings.interlaced)/8)+6250)/12500)*1000;

        // Switch to integer mult if possible
        if (!vm_conf->framelock) {
            if ((pclk_hz >= src_clk_hz) && (pclk_hz % src_clk_hz == 0))
                vm_conf->si_pclk_mult = (pclk_hz/src_clk_hz);
            else if ((pclk_hz < src_clk_hz) && (src_clk_hz % pclk_hz == 0))
                vm_conf->si_pclk_mult = (-1)*((src_clk_hz/pclk_hz)-1);
        }
    }

    pclk_hz *= vm_conf->tx_pixelrep+1;

    return pclk_hz;
}

/*
 * Downscaler modes.
 *
 * The output is frame-locked to the input and uses the TVP7002 sampling clock
 * as is (pclk_out = pclk_in), so there is no frame buffer. Only one out of
 * every (vdec+1) input lines is stored. For an exact frame lock
 *     smp_h_total * input_v_total == out_h_total * out_v_total
 * and the output line rate is ~15.7kHz. With hfilt the ADC samples
 * (hfilt+1)*smp_h_total per line and each group of (hfilt+1) samples is
 * averaged (box filter against aliasing); out_h_total scales accordingly.
 * Output lines are kept <= 4095 clocks: longer lines (possible in the FPGA,
 * whose horizontal fields are 13 bits wide) gave a black screen in testing,
 * most likely a 12-bit limit in the HDMI transmitter or the HDMI-VGA DAC.
 *
 * When an output line lasts a non-integer number of input lines, reading
 * drifts relative to writing over the frame; the line buffer layout is chosen
 * deep enough to absorb it (worst case 720p: ~11 stored lines -> 16 x 512).
 * v_start / out_v_backporch were chosen with a pixel-exact simulation of the
 * read/write timing (see commit history) to keep >= 2 lines of margin.
 */
typedef struct {
    HDMI_Video_Type vic;        // source preset
    uint16_t in_v_total;
    uint16_t smp_h_total;       // stored samples per input line (ADC: x(hfilt+1))
    uint16_t out_h_total;
    uint16_t out_v_total;
    uint8_t vdec;               // stored line = every (vdec+1)th input line (1..4)
    uint8_t x_rpt;              // each sample lasts (x_rpt+1) output clocks
    uint8_t lb_layout;          // 1 = 8x1024, 2 = 16x512, 3 = runtime stride (full resolution)
    uint8_t out_v_backporch;    // output active starts at line 3+out_v_backporch
    uint8_t v_start;            // first line of the first group (before "Line phase")
    uint16_t in_crop;           // source pixels cropped on each side
    uint8_t ar43;               // HD format: 0 = 16:9, 1 = center 4:3, 2 = wide (720p: 180p, 1080p: 216p)
    uint8_t hfilt;              // ADC samples averaged per stored sample: 0 = 1, 1 = 2, 2 = 4, 3 = 3
    uint8_t ilace;              // 480i output: one field per input frame
    uint8_t fB_st, fB_n;        // 480i field B: line in group / lines averaged
    char name[11];
    // Output on a separate clock from the FPGA PLL: pclk_out = pclk_in *
    // pll_m / (pll_n * pll_c), an exact ratio so that the output stays frame
    // locked (computed by ds_pll_solve() for the full resolution rows).
    uint16_t pll_n, pll_m, pll_c;
    // Full resolution (lb_layout 3): every ADC sample (or group of hfilt
    // samples) is a stored pixel, shown for x_rpt+1 clocks on the PLL clock;
    // the line buffer timing is computed (t0_auto) instead of out_v_backporch.
    uint8_t t0_auto;
    // full resolution: store RGB, one pixel per word, instead of YCbCr 4:2:2
    // (480p / VGA: few lines of read/write drift, the line buffer has room)
    uint8_t rgb444;
    // 720p "Wide": 180 active lines centered in the 240 line field
    uint8_t wide;
    // 50Hz source, 288p (310-314 lines) / 576i (625 lines) output
    uint8_t pal;
    // stored rows per field when not the full 240 / 288 (0 = default)
    uint16_t rows;
    // line buffer in YCbCr 4:1:1 (8 pixels in 3 words; >= 2 clocks per pixel)
    uint8_t c411;
} ds_mode_t;

static const ds_mode_t ds_modes[] = {
    // 720p60: 655x750 -> 1875x262 (2.86 input lines per output line)
    /* ---- 240p output (262 lines) ---- */
    { HDMI_720p60,      750,  655, 1875, 262, 2, 2, 2, 19, 0,   0, 0, 0, 0, 0, 0, "720p>240p" },
    //   filtered: ADC 1310/line (59MHz, 2x), 508 samples averaged over 2, 3750x262
    { HDMI_720p60,      750,  655, 3750, 262, 2, 5, 2, 19, 0,   0, 0, 1, 0, 0, 0, "720p>240p" },
    // 720p60 center 4:3 (960 of 1280 px): 786x750 -> 2250x262
    { HDMI_720p60,      750,  786, 2250, 262, 2, 3, 2, 19, 0, 160, 1, 0, 0, 0, 0, "720p 4:3" },
    //   filtered: ADC 1310/line (59MHz, 2x), 381 samples averaged over 2, 3750x262
    { HDMI_720p60,      750,  655, 3750, 262, 2, 7, 2, 19, 0, 160, 1, 1, 0, 0, 0, "720p 4:3" },
    // 480p60 (DTV): 1048x525 -> 2100x262 (2.004 input lines per output line)
    { HDMI_480p60,      525, 1048, 2100, 262, 1, 1, 3, 17, 0,   0, 0, 0, 0, 0, 0, "480p>240p" },
    //   (no filtered 240p variant: 2x oversampling would need >4095 clocks/line)
    // 640x480 VGA: same timing, odd lines
    { HDMI_640x480p60,  525, 1048, 2100, 262, 1, 1, 3, 17, 0,   0, 0, 0, 0, 0, 0, "VGA>240p" },

    /* ---- 480i output (525 lines, 2 fields of 262.5) ----
     * Each input frame becomes one field: field A (upper) takes line 0 of every
     * group, field B line 1 (480p) or the average of lines 1-2 (720p: rows at
     * 1.5-line spacing). Frame lock: ADC * in_v_total * 2 == out_h * 525. */
    // 720p: 658x750 -> 1880 (2.857 input lines per output line)
    { HDMI_720p60,      750,  658, 1880, 525, 2, 2, 2, 19, 0,   0, 0, 0, 1, 2, 2, "720p>480i" },
    { HDMI_720p60,      750,  658, 3760, 525, 2, 5, 2, 19, 0,   0, 0, 1, 1, 2, 2, "720p>480i" },
    { HDMI_720p60,      750,  784, 2240, 525, 2, 3, 2, 19, 0, 160, 1, 0, 1, 2, 2, "4:3>480i" },
    { HDMI_720p60,      750,  658, 3760, 525, 2, 7, 2, 19, 0, 160, 1, 1, 1, 2, 2, "4:3>480i" },
    // Full resolution ("Sampling: Full res"): every ADC sample is a stored
    // pixel (YCbCr 4:2:2), sampled at the source pixel clock. The output clock
    // comes from the FPGA PLL at an exact ratio of the ADC clock; out_h_total
    // (and for 240p the number of lines, 260..264) is chosen by
    // ds_pll_solve() for the requested "Width". out_h below is unused.
    // 720p: 1248 pixels (16 cropped per side), 2 clocks per pixel
    { HDMI_720p60,      750, 1650, 3125, 262, 2, 1, 3,  0, 0,  16, 0, 0, 0, 0, 0, "720p>240pF", 0, 0, 0, 1 },
    { HDMI_720p60,      750, 1650, 3125, 525, 2, 1, 3,  0, 0,  16, 0, 0, 1, 2, 2, "720p>480iF", 0, 0, 0, 1 },
    // 720p center 4:3: 960 pixels, 3 clocks per pixel
    { HDMI_720p60,      750, 1650, 3600, 262, 2, 2, 3,  0, 0, 160, 1, 0, 0, 0, 0, "4:3>240pF",  0, 0, 0, 1 },
    { HDMI_720p60,      750, 1650, 3600, 525, 2, 2, 3,  0, 0, 160, 1, 0, 1, 2, 2, "4:3>480iF",  0, 0, 0, 1 },
    // 480p: 720 pixels (858/line, 27MHz), VGA: 640 pixels (800/line, 25.175MHz), 3 clocks per pixel
    //   (RGB 4:4:4: one word per pixel, 11-12 line buffer slots)
    { HDMI_480p60,      525,  858, 2700, 262, 1, 2, 3,  0, 0,   0, 0, 0, 0, 0, 0, "480p>240pF", 0, 0, 0, 1, 1 },
    { HDMI_480p60,      525,  858, 2700, 525, 1, 2, 3,  0, 0,   0, 0, 0, 1, 1, 1, "480p>480iF", 0, 0, 0, 1, 1 },
    { HDMI_640x480p60,  525,  800, 2400, 262, 1, 2, 3,  0, 0,   0, 0, 0, 0, 0, 0, "VGA>240pF",  0, 0, 0, 1, 1 },
    { HDMI_640x480p60,  525,  800, 2400, 525, 1, 2, 3,  0, 0,   0, 0, 0, 1, 1, 1, "VGA>480iF",  0, 0, 0, 1, 1 },
    // "Wide" modes: line buffer in YCbCr 4:1:1 (8 pixels in 3 words, one
    // chroma sample per 4 pixels), 3 ADC samples averaged per pixel.
    // 720p "Wide" (whole 16:9 picture letterboxed): 1224x720 -> 408x180 (4
    // lines averaged per row, 28 pixels cropped per side), 5 clocks per pixel,
    // 550 stored samples per line. 53 line buffer slots (153 words per row)
    // absorb the drift of 720 input lines read in 180 output lines.
    { HDMI_720p60,      750,  550, 2730, 262, 3, 4, 3,  0, 0,  28, 2, 3, 0, 0, 0, "720pW>180p", 0, 0, 0, 1, 0, 1, 0, 0, 1 },
    //   480i: 360 rows, 180 per field; field A = lines 0-1 of each group of 4,
    //   field B = lines 2-3 (averaged, or lines 0 / 2 without line blending)
    { HDMI_720p60,      750,  550, 2730, 525, 3, 4, 3,  0, 0,  28, 2, 3, 1, 3, 2, "720pW>480i", 0, 0, 0, 1, 0, 1, 0, 0, 1 },
    // 1080p "Wide" (216p): 5 lines per output line, 1080 -> 216 rows (90% of
    // the field height) with the whole 1920 pixel width (the picture is
    // squeezed horizontally): 640 pixels, 3 clocks per pixel; 733.33 stored
    // samples per line (smp_h_total 733 plus h_total_adj). Each row takes 74us
    // of input (more than an output line): ~31 rows of read/write drift, 34
    // line buffer slots (240 words per row).
    //   240p: lines 1-4 of each group of 5 averaged (or line 2); 480i: field
    //   A = lines 0-2, field B = lines 2-4 (or lines 1 / 3)
    { HDMI_1080p60,    1125,  733, 2730, 262, 4, 2, 3,  0, 0,   0, 2, 3, 0, 0, 0, "1080>216p",  0, 0, 0, 1, 0, 1, 0, 216, 1 },
    { HDMI_1080p60,    1125,  733, 2730, 525, 4, 2, 3,  0, 0,   0, 2, 3, 1, 0, 0, "1080>216i",  0, 0, 0, 1, 0, 1, 0, 216, 1 },
    // 1080p60 "4:1 overscan": the 1708x960 window in the middle (60 lines and
    // 106 pixels cropped per side, ~89% of the picture like the overscan of a
    // TV), 4 lines per output line, 2 ADC samples averaged per stored pixel
    // (ADC at the full 148.5MHz pixel clock, 1100 stored samples per line),
    // 854 stored pixels, 2 clocks per pixel. 480i: field A = lines 0-1,
    // field B = lines 2-3 of each group.
    { HDMI_1080p60,    1125, 1100, 2700, 262, 3, 1, 3,  0, 60, 106, 0, 1, 0, 0, 0, "1080p>240p", 0, 0, 0, 1 },
    { HDMI_1080p60,    1125, 1100, 2700, 525, 3, 1, 3,  0, 60, 106, 0, 1, 1, 3, 2, "1080p>480i", 0, 0, 0, 1 },
    // 1080p center 4:3: the 1280x960 window, 640 stored pixels, 3 clocks per pixel
    { HDMI_1080p60,    1125, 1100, 2400, 262, 3, 2, 3,  0, 60, 320, 1, 1, 0, 0, 0, "1080 4:3p",  0, 0, 0, 1 },
    { HDMI_1080p60,    1125, 1100, 2400, 525, 3, 2, 3,  0, 60, 320, 1, 1, 1, 3, 2, "1080 4:3i",  0, 0, 0, 1 },
    // 1280x960 (VESA DMT timing, 1800x1000, 108MHz): 4 lines per output line
    // (exactly 240 rows: 320x240 content scaled x4), 2 ADC samples averaged
    // per stored pixel, 640 pixels, 3 clocks per pixel; 480i: field A = lines
    // 0-1, field B = lines 2-3 of each group
    { HDMI_Unknown,    1000,  900, 2400, 262, 3, 2, 3,  0, 0,   0, 0, 1, 0, 0, 0, "960p>240p",  0, 0, 0, 1 },
    { HDMI_Unknown,    1000,  900, 2400, 525, 3, 2, 3,  0, 0,   0, 0, 1, 1, 3, 2, "960p>480i",  0, 0, 0, 1 },
    // 576p50: 720 pixels (864/line, 27MHz), 3 clocks per pixel, RGB 4:4:4 as
    // 480p; 288p (310-314 lines, 15.5-15.7kHz) or 576i (625 lines)
    { HDMI_576p50,      625,  864, 2700, 312, 1, 2, 3,  0, 0,   0, 0, 0, 0, 0, 0, "576p>288pF", 0, 0, 0, 1, 1, 0, 1 },
    { HDMI_576p50,      625,  864, 2700, 625, 1, 2, 3,  0, 0,   0, 0, 0, 1, 1, 1, "576p>576iF", 0, 0, 0, 1, 1, 0, 1 },
    // 480p: native sampling (858/line, 720 active), exactly 2 input lines per output line
    { HDMI_480p60,      525,  858, 1716, 525, 1, 1, 1, 16, 0,   0, 0, 0, 1, 1, 1, "480p>480i" },
    { HDMI_480p60,      525,  858, 3432, 525, 1, 3, 1, 16, 0,   0, 0, 1, 1, 1, 1, "480p>480i" },
    // VGA: native sampling (800/line, 640 active)
    { HDMI_640x480p60,  525,  800, 1600, 525, 1, 1, 1, 16, 0,   0, 0, 0, 1, 1, 1, "VGA>480i" },
    { HDMI_640x480p60,  525,  800, 3200, 525, 1, 3, 1, 16, 0,   0, 0, 1, 1, 1, 1, "VGA>480i" },
};

#define DS_OUT_V_SYNCLEN    3
#define DS_OUT_V_ACTIVE     240
#define DS_OUT_V_FPORCH     3   // blank lines before vsync (pre-equalizing pulses)
#define DS_OUT_V_BPORCH_MIN 4   // blank lines after vsync (post-equalizing pulses + 1)

#define DS_OUT_H_TOTAL_MAX  4095
#define DS_PLL_OUT_MAX_KHZ  66000   // see pclk_5x in ossc.sdc
// Cyclone IV PLL: phase detector input >= 5MHz (datasheet). The scan chain
// reuses the charge pump / loop filter bits of the stock low bandwidth
// configs (M = 100-120), so M is kept in a similar range: v0.16 used
// M = 262 at 3.6MHz for VGA, which showed shimmer.
#define DS_PLL_PFD_MIN_KHZ  5000
#define DS_PLL_M_MAX        200
#define DS_OUT_V_ACTIVE_PAL 288
// active lines per output field, and stored rows (Wide: 180 of them)
#define DS_OUT_V_FIELD_ACT(m) ((m)->pal ? DS_OUT_V_ACTIVE_PAL : DS_OUT_V_ACTIVE)
#define DS_OUT_V_ROWS(m)    ((m)->rows ? (m)->rows : ((m)->wide ? 180 : DS_OUT_V_FIELD_ACT(m)))
// output lines per frame in 480i / 576i
#define DS_OUT_ILACE_LINES(m) ((m)->pal ? 625 : 525)
// 480p / VGA -> 480i flicker filter: RGB rows with groups of 2 lines
#define DS_VFLT(m, cc)      ((m)->ilace && ((m)->vdec == 1) && (m)->rgb444 && (cc)->ds_flicker && ((cc)->ds_flicker <= 4))
// line buffer words per stored line / number of lines (layout 3)
#define DS_LB_STRIDE(m, w)  ((m)->rgb444 ? (w) : ((m)->c411 ? ((w)/8)*3 : (((w)+1)/2)))
#define DS_LB_SLOTS(st)     (((8192/(st)) > 63) ? 63 : (8192/(st)))
static uint16_t ds_framesync_line, ds_v_active, ds_hofs;

// ADC samples per stored sample
static uint16_t ds_hskip1(const ds_mode_t *m) {
    static const uint8_t n[4] = {1, 2, 4, 3};
    return n[m->hfilt & 3];
}

static uint8_t ds_mode_sampling(const ds_mode_t *m) {
    return m->t0_auto ? 1 : 0;
}

// Row for this source preset: by VIC, or by total lines for presets without
// one (1280x960)
static int ds_row_match(const ds_mode_t *m, mode_data_t *preset) {
    if (m->vic != preset->vic)
        return 0;
    return (m->vic != HDMI_Unknown) || (preset->timings.v_total == m->in_v_total);
}

// "HD format" applies to these sources
static int ds_hd_vic(HDMI_Video_Type vic) {
    return (vic == HDMI_720p60) || (vic == HDMI_1080p60);
}

static const ds_mode_t* find_ds_mode(avconfig_t *cc, mode_data_t *preset, mode_data_t *vm_in) {
    unsigned i;
    const ds_mode_t *match = NULL;
    uint8_t want_hfilt = !cc->ds_hfilt_off;
    uint8_t ar;
    // wanted output: 0 = 240p, 1 = 480i
    uint8_t want[2];
    unsigned pass, npass = 0;

    uint8_t pal;

    // 60Hz sources, and 50Hz for the PAL rows (576p)
    if (vm_in->timings.interlaced)
        return NULL;
    if ((vm_in->timings.v_hz_x100 >= 5800) && (vm_in->timings.v_hz_x100 <= 6200))
        pal = 0;
    else if ((vm_in->timings.v_hz_x100 >= 4800) && (vm_in->timings.v_hz_x100 <= 5200))
        pal = 1;
    else
        return NULL;
    // "HD format" (720p and 1080p; Wide: 180p for 720p, 216p for 1080p)
    ar = cc->ds_720p_ar;

    if (cc->ds_480i)
        want[npass++] = 1;
    want[npass++] = 0;

    // pick the variant with the requested output and filter setting; fall
    // back to another filter setting, then to the next output type
    for (pass=0; pass<npass; pass++) {
        // "Sampling": 1 = full resolution rows (t0_auto), preferred when
        // selected, otherwise standard rows
        if (cc->ds_native) {
            for (i=0; i<sizeof(ds_modes)/sizeof(ds_mode_t); i++) {
                if (ds_row_match(&ds_modes[i], preset) && (ds_modes[i].pal == pal) && (ds_mode_sampling(&ds_modes[i]) == 1) &&
                    (ds_modes[i].ilace == want[pass]) &&
                    (!ds_hd_vic(ds_modes[i].vic) || (ds_modes[i].ar43 == ar)))
                    return &ds_modes[i];
            }
        }
        for (i=0; i<sizeof(ds_modes)/sizeof(ds_mode_t); i++) {
            if (ds_row_match(&ds_modes[i], preset) && (ds_modes[i].pal == pal) &&
                ((ds_mode_sampling(&ds_modes[i]) == 0) || ds_modes[i].wide) &&     // "Wide" exists in full resolution only
                (ds_modes[i].ilace == want[pass]) &&
                (!ds_hd_vic(ds_modes[i].vic) || (ds_modes[i].ar43 == ar))) {
                if (!!ds_modes[i].hfilt == want_hfilt)
                    return &ds_modes[i];
                if (!match)
                    match = &ds_modes[i];
            }
        }
        if (match)
            return match;
    }
    return NULL;
}

// Output clock / line length for the full resolution modes. The PLL makes
// pclk_out = pclk_in * M / (N * C) and the output must stay frame locked, so
//   out_h * (output lines per input frame) / (h_total_in * v_total_in) = M / (N * C)
// exactly. Only some out_h have a solution with M, C <= 510 in the PLL
// limits (VCO 600-1300MHz, phase detector 5-10MHz, M 50-200 near the low
// bandwidth configs whose loop filter settings are reused, output 25-66MHz); for 240p the field may also have 260-264 lines
// (15.6-15.9kHz) to find more of them, unless fixed_lines asks for 262 only
// (exact standard height, coarser width steps). Picks the line length whose active
// part (act clocks) is closest to target_pm (per mille of the line).
static uint32_t ds_gcd(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int ds_t0_window_ok(const ds_mode_t *m, avconfig_t *cc, mode_data_t *vm_in, uint16_t w, uint16_t rpt);

// Line written in each group (st, 0..vdec) and number of lines averaged into
// it (n, ending at st), for field A (240p: every field) and field B (480i).
static void ds_field_lines(const ds_mode_t *m, avconfig_t *cc, int *st_a, int *n_a, int *st_b, int *n_b) {
    if (DS_VFLT(m, cc)) {
        // flicker filter: rows written at the line after their center line,
        // field B groups one line later (see linebuf_top)
        *st_a = 1;
        *st_b = 2;
        *n_a = *n_b = 1;
        return;
    }
    if (m->vdec == 4) {
        // groups of 5 lines (1080p Wide 216p). 240p: lines 1-4 or line 2;
        // 480i: A = lines 0-2, B = lines 2-4 (or lines 1 / 3)
        if (m->ilace) {
            *st_a = cc->ds_vblend_off ? 1 : 2;
            *st_b = cc->ds_vblend_off ? 3 : 4;
            *n_a = *n_b = cc->ds_vblend_off ? 1 : 3;
        } else {
            *st_a = *st_b = cc->ds_vblend_off ? 2 : 4;
            *n_a = *n_b = cc->ds_vblend_off ? 1 : 4;
        }
    } else if (m->ilace && (m->vdec == 3)) {
        // groups of 4 lines (Wide, 1080p): A = lines 0-1, B = lines 2-3
        *st_a = cc->ds_vblend_off ? 0 : 1;
        *n_a = cc->ds_vblend_off ? 1 : 2;
        *st_b = cc->ds_vblend_off ? 2 : 3;
        *n_b = *n_a;
    } else if (m->ilace) {
        *st_a = 0;
        *n_a = 1;
        *st_b = m->fB_st;
        *n_b = m->fB_n;
    } else if (!cc->ds_vblend_off) {
        // average the whole group, written at its last line
        *st_a = *st_b = m->vdec;
        *n_a = *n_b = m->vdec+1;
    } else {
        // the middle line of the group (720p: line 1 of 3, 480p: line 0 of 2)
        *st_a = *st_b = m->vdec/2;
        *n_a = *n_b = 1;
    }
}

static int ds_pll_solve(const ds_mode_t *row, avconfig_t *cc, mode_data_t *vm_in, uint16_t w, uint16_t rpt,
                        uint32_t fin_khz, uint32_t hin, uint32_t vin, uint8_t ilace, uint32_t act, uint32_t target_pm,
                        uint8_t fixed_lines, uint16_t *oh_o, uint16_t *lines_o, uint16_t *n_o, uint16_t *m_o, uint16_t *c_o) {
    ds_mode_t cand;
    static const uint16_t lines_ntsc[] = {262, 264, 260, 261, 263};
    static const uint16_t lines_pal[] = {312, 314, 310, 311, 313};
    const uint16_t *lines_opt = row->pal ? lines_pal : lines_ntsc;
    uint32_t ilace_lines = DS_OUT_ILACE_LINES(row);
    uint32_t oh, oh_min, oh_max, li_i, best_err = 0xffffffff;
    uint32_t n_lines = (ilace || fixed_lines) ? 1 : sizeof(lines_ntsc)/sizeof(lines_ntsc[0]);
    int found = 0;

    oh_min = (act*1000 + 879)/880;      // active <= 88% of the line
    oh_max = (act*1000)/600;            // active >= 60%
    if (oh_max > DS_OUT_H_TOTAL_MAX)
        oh_max = DS_OUT_H_TOTAL_MAX;

    for (oh=oh_min; oh<=oh_max; oh++) {
        for (li_i=0; li_i<n_lines; li_i++) {
            uint32_t fo2 = ilace ? ilace_lines : 2*lines_opt[li_i];
            uint32_t hfreq = ((uint32_t)vm_in->timings.v_hz_x100*fo2)/200;     // output line rate (Hz)
            if ((hfreq < DS_HFREQ_MIN) || (hfreq > DS_HFREQ_MAX))
                continue;
            uint32_t num = oh*fo2, den = 2*hin*vin, g = ds_gcd(num, den), k, n;
            uint32_t err = (act*1000)/oh;

            err = (err > target_pm) ? (err - target_pm) : (target_pm - err);
            err = err*4 + li_i;     // prefer 262 lines on ties
            if (err >= best_err)
                continue;
            num /= g;
            den /= g;
            for (k=1; num*k <= DS_PLL_M_MAX; k++) {
                uint32_t m = num*k, nd = den*k, sol = 0;
                if (m < 50)
                    continue;
                for (n=1; n<=64; n++) {
                    uint32_t c, pfd, vco, out;
                    if (nd % n)
                        continue;
                    c = nd/n;
                    pfd = fin_khz/n;
                    vco = (fin_khz*m)/n;
                    out = vco/c;
                    if ((c > 510) || (pfd < DS_PLL_PFD_MIN_KHZ) || (pfd > 10000) || (vco < 600000) || (vco > 1300000) ||
                        (out < 25000) || (out > DS_PLL_OUT_MAX_KHZ))
                        continue;
                    // the line buffer must absorb the read/write drift of this line rate
                    memcpy(&cand, row, sizeof(ds_mode_t));
                    cand.out_h_total = oh;
                    cand.out_v_total = ilace ? ilace_lines : lines_opt[li_i];
                    if (!ds_t0_window_ok(&cand, cc, vm_in, w, rpt))
                        break;
                    *oh_o = oh;
                    *lines_o = ilace ? ilace_lines : lines_opt[li_i];
                    *n_o = n;
                    *m_o = m;
                    *c_o = c;
                    best_err = err;
                    found = 1;
                    sol = 1;
                    break;
                }
                if (sol)
                    break;
            }
        }
    }
    return found ? 0 : -1;
}

// Line buffer timing of the full resolution modes. Times are counted from the
// input frame start in units of 1/(2*Vin*Hin) output clocks, so that input
// pixels/lines and output clocks/lines are all integers. Each stored line k
// (rows 0..239) is written while input line Y0+(vdec+1)k+st+p arrives, and
// read during output line T0+k; its buffer is reused by line k+slots. T0 is
// put in the middle of the window that keeps every pixel written before it
// is read (worst at the last line, first pixel) and read before it is
// overwritten (worst at the first line, last pixel). In 480i the even field
// is read half a line later relative to its input frame.
#define DS_T0_MARGIN_OC 24      // pipeline delays, output clocks
static void ds_t0_window(const ds_mode_t *m, avconfig_t *cc, mode_data_t *vm_in, int32_t out_offset, uint16_t w, uint16_t rpt,
                         int64_t *lo_o, int64_t *hi_o, int64_t *u_oline_o, int64_t *u_oc_o) {
    int64_t hin = vm_in->timings.h_total;
    int64_t vin = m->in_v_total;
    int64_t fo2 = m->ilace ? m->out_v_total : 2*m->out_v_total;     // 2 x output lines per input frame
    int64_t u_ipx = (int64_t)m->out_h_total*fo2;
    int64_t u_iline = u_ipx*hin;
    int64_t u_oc = 2*vin*hin;
    int64_t u_oline = u_oc*m->out_h_total;
    int64_t y0 = vm_in->timings.v_synclen + vm_in->timings.v_backporch - 1 + m->v_start;
    // (4:1:1: a word is written with the 4th / 8th pixel of its group)
    int64_t off_in = vm_in->timings.h_synclen + vm_in->timings.h_backporch + (m->c411 ? 7 : 0);
    int64_t off_out = out_offset;  // output hsync + back porch
    int64_t g = m->vdec+1, k = DS_OUT_V_ROWS(m), d = DS_LB_SLOTS(DS_LB_STRIDE(m, w));
    int64_t p = cc->ds_vphase % (m->vdec+1);
    int64_t eps = DS_T0_MARGIN_OC*u_oc;
    int64_t lo = INT64_MIN, hi = INT64_MAX;
    int st_a, n_a, st_b, n_b, f, i;

    ds_field_lines(m, cc, &st_a, &n_a, &st_b, &n_b);

    // Row r is written while input line y0+g*r+st arrives (first pixel at
    // off_in) and read during output line T0+r (first pixel at off_out); its
    // buffer is reused by row r+d. Both conditions are linear in r, so the
    // worst case is the first or the last row: the last row when reading is
    // faster than writing (720p: 3 input lines per 2.86), the first row when
    // writing is faster (1080p 4:1: 4 input lines per 4.29). Within a line the
    // reader is always slower (an output line spans several input lines), so
    // the worst pixel is the first one for "written before read" and the last
    // one for "read before overwritten". 480i field B is read half a line
    // later relative to its input frame.
    for (f=0; f<(m->ilace ? 2 : 1); f++) {
        int64_t st = f ? st_b : st_a;
        int64_t ofs = f ? u_oline/2 : 0;
        for (i=0; i<2; i++) {
            int64_t r = i ? (k-1) : 0;
            int64_t l = (y0 + g*r + st + p)*u_iline + off_in*u_ipx - r*u_oline - off_out*u_oc + eps - ofs;
            int64_t h = (y0 + g*(r+d) + st + p)*u_iline + (off_in+w-1)*u_ipx - r*u_oline - (off_out + (int64_t)rpt*(w-1))*u_oc - eps - ofs;
            if (l > lo) lo = l;
            if (h < hi) hi = h;
        }
    }
    *lo_o = lo;
    *hi_o = hi;
    *u_oline_o = u_oline;
    *u_oc_o = u_oc;
}

static int32_t ds_calc_t0(const ds_mode_t *m, avconfig_t *cc, mode_data_t *vm_in, int32_t out_offset, uint16_t w, uint16_t rpt) {
    int64_t lo, hi, u_oline, u_oc;

    ds_t0_window(m, cc, vm_in, out_offset, w, rpt, &lo, &hi, &u_oline, &u_oc);
    return (int32_t)(((lo + hi)/2)/u_oc);
}

// The window width does not depend on the output H position or the line
// phase (they shift both ends), only on the line length / number of lines:
// require >= 0.3 lines of margin on each side.
static int ds_t0_window_ok(const ds_mode_t *m, avconfig_t *cc, mode_data_t *vm_in, uint16_t w, uint16_t rpt) {
    int64_t lo, hi, u_oline, u_oc;

    ds_t0_window(m, cc, vm_in, 0, w, rpt, &lo, &hi, &u_oline, &u_oc);
    return (hi - lo) >= (u_oline*6)/10;
}


static int setup_ds_mode(const ds_mode_t *m, avconfig_t *cc, mode_data_t *vm_in, mode_data_t *vm_out, vm_proc_config_t *vm_conf) {
    uint16_t in_h_total = vm_in->timings.h_total;
    uint16_t smp_active, out_h_synclen, out_h_bp_min, out_h_fp_min, out_active_max, out_active, rpt;
    ds_mode_t me;   // full resolution: row with the computed output timing

    // Optional source crop (e.g. center 4:3 of 16:9): narrower capture window
    vm_in->timings.h_backporch += m->in_crop;
    vm_in->timings.h_active -= 2*m->in_crop;

    // Resample horizontal timings to smp_h_total samples per line (full
    // resolution: exactly 1 / ds_hskip1 of the ADC samples; 1080p Wide has
    // 366.67 stored samples per line)
    if (m->t0_auto) {
        uint16_t sk = ds_hskip1(m);
        smp_active = vm_in->timings.h_active/sk;
        vm_in->timings.h_synclen = (vm_in->timings.h_synclen + sk/2)/sk;
        vm_in->timings.h_backporch = (vm_in->timings.h_backporch + sk/2)/sk;
    } else {
        smp_active = (vm_in->timings.h_active*m->smp_h_total + in_h_total/2)/in_h_total;
        vm_in->timings.h_synclen = (vm_in->timings.h_synclen*m->smp_h_total + in_h_total/2)/in_h_total;
        vm_in->timings.h_backporch = (vm_in->timings.h_backporch*m->smp_h_total + in_h_total/2)/in_h_total;
    }
    vm_in->timings.h_active = smp_active;
    vm_in->timings.h_total = m->smp_h_total;
    // fractional samples per line (Wide: 1650 ADC samples = 4 x 412.5), in
    // the 1/20 sample units of h_total_adj (see pll_h_total)
    {
        uint16_t skip1 = ds_hskip1(m);
        uint16_t rem = (in_h_total > skip1*m->smp_h_total) ? (in_h_total - skip1*m->smp_h_total) : 0;
        vm_in->timings.h_total_adj = m->t0_auto ? (rem*20)/skip1 : 0;
    }

    memcpy(vm_out, vm_in, sizeof(mode_data_t));

    rpt = m->x_rpt+1;

    if (m->t0_auto) {
        // Frame lock uses the measured lines per input frame: a source a few
        // lines off the standard total would otherwise drift and be resynced
        // over and over (visible as stutter).
        memcpy(&me, m, sizeof(ds_mode_t));
        if ((vm_in->timings.v_total != m->in_v_total) && (abs((int)vm_in->timings.v_total - (int)m->in_v_total) <= 8))
            me.in_v_total = vm_in->timings.v_total;
        m = &me;
    }

    if (m->t0_auto) {
        // output line length / PLL ratio for the requested width
        uint32_t fin_khz = (uint32_t)(((uint64_t)in_h_total*m->in_v_total*vm_in->timings.v_hz_x100)/100000);
        // "Width": 100% = 80% of the line
        uint32_t target_pm = 800 + 8*((int)cc->ds_width - SIGNED_NUMVAL_ZERO);
        uint16_t oh, lines, n, mm, c;

        if (ds_pll_solve(m, cc, vm_in, smp_active, rpt, fin_khz, in_h_total, m->in_v_total, m->ilace, (uint32_t)smp_active*rpt, target_pm,
                         cc->ds_fixlines, &oh, &lines, &n, &mm, &c) != 0) {
            // no 262 line solution: fall back to 260-264 lines
            if (!cc->ds_fixlines ||
                (ds_pll_solve(m, cc, vm_in, smp_active, rpt, fin_khz, in_h_total, m->in_v_total, m->ilace, (uint32_t)smp_active*rpt, target_pm,
                              0, &oh, &lines, &n, &mm, &c) != 0))
                return -1;
        }
        me.out_h_total = oh;
        me.out_v_total = lines;
        me.pll_n = n;
        me.pll_m = mm;
        me.pll_c = c;
        m = &me;
    }

    // 15kHz horizontal timings, in fractions of the 63.6us line:
    // hsync 4.7us, back porch >= 5.15us, front porch >= 1.4us
    out_h_synclen = (m->out_h_total*74 + 500)/1000;   // 9-bit field (up to 511)
    out_h_bp_min = (m->out_h_total*81 + 500)/1000;
    out_h_fp_min = (m->out_h_total*22 + 500)/1000;
    out_active_max = ((m->out_h_total - out_h_synclen - out_h_bp_min - out_h_fp_min)/rpt)*rpt;
    out_active = smp_active*rpt;
    if (m->t0_auto) {
        // every stored pixel is shown; the blanking is split 3:1 between
        // back and front porch (the H. position limits still apply)
        out_active_max = out_active;
        out_h_bp_min = ((m->out_h_total - out_h_synclen - out_active)*3)/4;
    } else if (out_active > out_active_max) {
        out_active = out_active_max;    // crop the sides evenly
    }

    vm_out->timings.h_total = m->out_h_total;
    vm_out->timings.h_active = out_active;
    vm_out->timings.h_synclen = (out_h_synclen > 255) ? 255 : out_h_synclen;  // info only
    vm_conf->ds_h_synclen = out_h_synclen;
    vm_out->timings.h_backporch = out_h_bp_min + (out_active_max-out_active)/2;
    // User horizontal position, in steps of one stored pixel. Keeps a back
    // porch >= 2.5us and a front porch >= 0.6us.
    {
        int hoff = ((int)cc->ds_hpos - SIGNED_NUMVAL_ZERO) * rpt;
        int bp = (int)vm_out->timings.h_backporch + hoff;
        int bp_lim = (m->out_h_total*40 + 500)/1000;
        int bp_max = (int)m->out_h_total - out_h_synclen - out_active - (m->out_h_total*10 + 500)/1000;

        if (bp < bp_lim)
            bp = bp_lim;
        if (bp > bp_max)
            bp = bp_max;
        if (bp > 1023)
            bp = 1023;          // 10-bit register field
        vm_out->timings.h_backporch = bp;
    }
    vm_out->timings.h_total_adj = 0;
    vm_out->timings.v_total = m->out_v_total;       // 525 / 625 for 480i / 576i (per frame)
    vm_out->timings.v_active = DS_OUT_V_FIELD_ACT(m);   // per field
    vm_out->timings.v_synclen = DS_OUT_V_SYNCLEN;
    // Vertical timing. Active video must end before the next vsync's
    // pre-equalizing pulses (3 lines): HDMI cannot carry sync changes during
    // active video, and monitors expect a vertical front porch. The line
    // buffer timing (when each output line is read relative to the input
    // frame) is set by out_v_backporch and is kept constant: a shorter back
    // porch (or "V. position" upwards) starts the output field later relative
    // to the input frame (framesync_line), a longer one earlier. Moving the
    // picture down beyond the front porch limit hides the bottom lines.
    {
        // Wide: 180 rows centered where the 240 rows would be (the extra
        // back / front porch lines are black)
        int rows = DS_OUT_V_ROWS(m);
        int pad = (DS_OUT_V_FIELD_ACT(m) - rows)/2;
        int field_lines = m->ilace ? (m->out_v_total/2) : m->out_v_total;
        int vbp_max = field_lines - DS_OUT_V_FPORCH - DS_OUT_V_SYNCLEN - rows;
        int vbp = ((m->t0_auto || (m->out_v_backporch > vbp_max)) ? (vbp_max - pad) : m->out_v_backporch) + ((int)cc->ds_vpos - SIGNED_NUMVAL_ZERO);
        int v_active = rows;
        int delay;
        int32_t hofs = 0;

        if (vbp < DS_OUT_V_BPORCH_MIN)
            vbp = DS_OUT_V_BPORCH_MIN;
        if (vbp > vbp_max)
            v_active -= vbp - vbp_max;

        if (m->t0_auto) {
            // output clocks from the input frame start to the start of the
            // output line with active row 0, then split into whole lines
            // (framesync_line) and a sub-line offset (hofs)
            int32_t t0c = ds_calc_t0(m, cc, vm_in, out_h_synclen + vm_out->timings.h_backporch, smp_active, rpt);
            int32_t delay_c = t0c - (int32_t)(DS_OUT_V_SYNCLEN + vbp)*m->out_h_total;
            delay = (delay_c >= 0) ? (delay_c + m->out_h_total - 1)/m->out_h_total : -((-delay_c)/m->out_h_total);
            hofs = delay*m->out_h_total - delay_c;
            if (hofs < 128) {
                hofs = 0;
            } else if (hofs > m->out_h_total-129) {
                delay--;
                hofs = 0;
            }
        } else {
            delay = (int)m->out_v_backporch - vbp;   // output field start after the input frame start, in lines
        }

        vm_out->timings.v_backporch = vbp;
        vm_out->timings.v_active = v_active;
        ds_v_active = v_active;
        ds_framesync_line = (delay > 0) ? (field_lines - delay) : -delay;
        ds_hofs = hofs;
    }
    vm_out->timings.interlaced = !!m->ilace;
    vm_out->type = VIDEO_SDTV;
    vm_out->ar.h = 4;
    vm_out->ar.v = 3;
    vm_out->vic = HDMI_Unknown;

    vm_conf->x_rpt = m->x_rpt;
    vm_conf->y_rpt = 0;
    vm_conf->h_skip = ds_hskip1(m) - 1;    // ADC samples per stored sample - 1
    vm_conf->x_size = out_active;
    vm_conf->y_size = ds_v_active;
    vm_conf->x_offset = 0;
    vm_conf->y_offset = 0;
    vm_conf->x_start_lb = (smp_active - out_active/rpt)/2;
    // Vertical line selection (see linebuf_top): y_start_lb = first line of
    // the first group, per field: line written in the group (st) and number
    // of lines averaged into it (n).
    // "Line phase" shifts the line groups by 0..vdec lines so that they can be
    // aligned with integer-scaled content (blending a group of identical
    // lines then gives exactly the unblended picture).
    vm_conf->ds_ilace = !!m->ilace;
    vm_conf->ds_fswap = 0;     // field order: upper rows in the odd field (verified)
    vm_conf->ds_vblend = !cc->ds_vblend_off && !m->ilace;
    vm_conf->y_start_lb = m->v_start + (cc->ds_vphase % (m->vdec+1));
    if (DS_VFLT(m, cc)) {
        // flicker filter (see linebuf_top): both fields write at group
        // position 1, field B groups start one line later. Rows are
        // a*L(c-1) + (16-2a)*L(c) + a*L(c+1), a = 1..4 sixteenths. The last
        // field B row is centered on line 479 and uses line 480 (blanking):
        // one more input line is captured.
        vm_conf->ds_field_cfg = 1 | (1<<2) | (1<<4) | (1<<6);
        vm_conf->ds_vflt = 1;
        vm_conf->ds_vflt_bofs = 1;
        vm_conf->ds_vflt_a = cc->ds_flicker;
        vm_in->timings.v_active += 1;
    } else {
        // n = 4 is coded as 0; bits 8/9: bit 2 of st (groups of 6)
        int st_a, n_a, st_b, n_b;
        ds_field_lines(m, cc, &st_a, &n_a, &st_b, &n_b);
        vm_conf->ds_field_cfg = (st_a & 3) | ((n_a & 3)<<2) | ((st_b & 3)<<4) | ((n_b & 3)<<6) |
                                (((st_a >> 2) & 1)<<8) | (((st_b >> 2) & 1)<<9);
    }
    vm_conf->ds_hfilt = m->hfilt;
    vm_conf->ds_vdec = m->vdec;
    vm_conf->ds_lb_layout = m->lb_layout;
    vm_conf->ds_csync = cc->ds_csync;

    vm_conf->framesync_line = ds_framesync_line;
    vm_conf->framelock = 1;
    vm_conf->si_pclk_mult = 1;
    vm_conf->ds_lb_stride = (m->lb_layout == 3) ? DS_LB_STRIDE(m, smp_active) : 0;
    vm_conf->ds_lb_slots = (m->lb_layout == 3) ? DS_LB_SLOTS(vm_conf->ds_lb_stride) : 0;
    vm_conf->ds_lb_rgb = (m->lb_layout == 3) && m->rgb444;
    vm_conf->ds_lb411 = (m->lb_layout == 3) && m->c411;
    vm_conf->ds_hofs = ds_hofs;
    vm_conf->ds_pll_n = m->pll_n;
    vm_conf->ds_pll_m = m->pll_m;
    vm_conf->ds_pll_c = m->pll_c;
    vm_conf->tx_pixelrep = 0;
    vm_conf->hdmitx_pixr_ifr = 0;

    strncpy(vm_out->name, m->name, 11);

    if (vm_out->vic == HDMI_Unknown)
        vm_out->vic = cc->default_vic;
    return 0;
}

// Passthrough (15kHz sources): "H. position" / "V. position" move the active
// window relative to the output syncs; the line rate, the number of lines and
// the pixels are unchanged.
//  - right: later DE (longer back porch), up to the front porch limit, then
//    the right side is cropped
//  - left: earlier DE, down to the back porch limit, then the left side is
//    cropped
//  The passthrough output already runs one line behind the input (frame
//  change detection), and the second 480i field half a line more, so up to
//  +-40 pixels are read after they are written and before the 2-line buffer
//  overwrites them.
//  - down / up: the output frame starts N lines earlier / later than the
//    input (framesync) and the active lines start N lines later / earlier
//    after the vsync, so that the line buffer timing is the same. The
//    vertical front porch of the source is kept (some TVs lose sync when the
//    active lines get closer to the vsync): moving down crops the bottom
//    lines
static void ds_pt_position(avconfig_t *cc, mode_data_t *vm_out, vm_proc_config_t *vm_conf) {
    int rpt = vm_conf->x_rpt+1;
    int hpos = cc->ds_hpos ? ((int)cc->ds_hpos - SIGNED_NUMVAL_ZERO) : 0;
    int vpos = cc->ds_vpos ? ((int)cc->ds_vpos - SIGNED_NUMVAL_ZERO) : 0;
    int h_total = vm_out->timings.h_total;
    int sync = vm_out->timings.h_synclen;
    int bp = vm_out->timings.h_backporch;
    int act = vm_out->timings.h_active;
    int bp_lim = (h_total*40 + 500)/1000;       // >= 2.5us
    int fp_lim = (h_total*10 + 500)/1000;       // >= 0.6us
    int field_lines = vm_out->timings.v_total >> vm_out->timings.interlaced;
    int vbp = vm_out->timings.v_backporch;
    int vact = vm_out->timings.v_active;
    int vsync = vm_out->timings.v_synclen;
    int delay;

    if (!hpos && !vpos)
        return;

    if (hpos > 0) {
        int nbp = bp + hpos*rpt;
        int over = (sync + nbp + act) - (h_total - fp_lim);
        if (nbp > 1023)
            nbp = 1023;     // 10-bit register field
        if (over > 0) {
            int c = (over + rpt - 1)/rpt;   // whole pixels cropped on the right
            act -= c*rpt;
            vm_conf->x_size = (vm_conf->x_size > c*rpt) ? (vm_conf->x_size - c*rpt) : 0;
        }
        bp = nbp;
    } else if (hpos < 0) {
        int nbp = bp + hpos*rpt;
        if (nbp < bp_lim) {
            int c = (bp_lim - nbp + rpt - 1)/rpt;   // whole pixels cropped on the left
            if (c > 255 - vm_conf->x_start_lb)
                c = 255 - vm_conf->x_start_lb;
            vm_conf->x_start_lb += c;
            act -= c*rpt;
            vm_conf->x_size = (vm_conf->x_size > c*rpt) ? (vm_conf->x_size - c*rpt) : 0;
            nbp += c*rpt;
        }
        bp = nbp;
    }
    vm_out->timings.h_backporch = bp;
    vm_out->timings.h_active = act;

    // vertical: back porch >= 3 lines, front porch kept
    int vfp = field_lines - vsync - vbp - vact;
    if (vfp < 1)
        vfp = 1;
    if (vbp + vpos < 3)
        vpos = 3 - vbp;
    vbp += vpos;
    if (vsync + vbp + vact > field_lines - vfp) {
        int c = (vsync + vbp + vact) - (field_lines - vfp);
        vact -= c;
        vm_conf->y_size = (vm_conf->y_size > c) ? (vm_conf->y_size - c) : 0;
    }
    vm_out->timings.v_backporch = vbp;
    vm_out->timings.v_active = vact;

    // output frame start after the input frame start, in lines
    delay = -vpos;
    vm_conf->framesync_line = (delay > 0) ? (field_lines - delay) : -delay;
}

int get_pure_lm_mode(avconfig_t *cc, mode_data_t *vm_in, mode_data_t *vm_out, vm_proc_config_t *vm_conf)
{
    int i, diff_lines, diff_v_hz_x100, mindiff_id=0, mindiff_lines=1000, mindiff_v_hz_x100=10000, x_rpt_decr=0, skip_hv_mult=0;
    mode_data_t *mode_preset;
    mode_flags valid_lm[] = { MODE_PT,   // downscaler firmware: plain passthrough only
                              (MODE_L2 | (MODE_L2<<cc->l2_mode)),
                              (MODE_L3_GEN_16_9<<cc->l3_mode),
                              (MODE_L4_GEN_4_3<<cc->l4_mode),
                              (MODE_L5_GEN_4_3<<cc->l5_mode),
                              (MODE_L6_GEN_4_3<<cc->l6_mode) };
    mode_flags target_lm, mindiff_lm;
    uint8_t pt_only = 0;
    uint8_t upsample2x = cc->upsample2x;

    // one for each video_group
    // Downscaler firmware: no line multiplication. Sources that are not
    // downscaled are passed through and then checked against the 15kHz limit.
    uint8_t* group_ptr[] = { &pt_only, &pt_only, &pt_only, &pt_only, &pt_only, &pt_only, &pt_only, &pt_only, &pt_only, &pt_only, &pt_only };

    for (i=0; i<num_video_modes_plm; i++) {
        mode_preset = &video_modes_plm[i];

        switch (mode_preset->group) {
            case GROUP_384P:
                //fixed Line2x/3x mode for 240x360p/400p
                valid_lm[2] = MODE_L3_GEN_16_9 | MODE_L3_GEN_4_3;
                valid_lm[3] = MODE_L2_240x360;
                valid_lm[4] = MODE_L3_240x360;
                if ((!vm_in->timings.h_total) && (mode_preset->timings.v_total == 449)) {
                    if ((video_modes_plm_default[i].timings.h_active == 720) && (video_modes_plm_default[i].timings.v_active == 400)) {
                        if (cc->s400p_mode == 0)
                            continue;
                    } else if ((video_modes_plm_default[i].timings.h_active == 640) && (video_modes_plm_default[i].timings.v_active == 400)) {
                        if (cc->s400p_mode == 1)
                            continue;
                    }
                }
                break;
            case GROUP_480I:
            case GROUP_576I:
                //fixed Line3x/4x mode for 480i and onwards
                valid_lm[2] = MODE_L3_GEN_16_9;
                valid_lm[3] = MODE_L4_GEN_4_3;
                break;
            case GROUP_480P:
                if (mode_preset->vic == HDMI_480p60) {
                    switch (cc->s480p_mode) {
                        case 0: // Auto
                            if (vm_in->timings.h_synclen > 82)
                                continue;
                            break;
                        case 1: // DTV 480p
                            break;
                        default:
                            continue;
                    }
                } else if (mode_preset->vic == HDMI_640x480p60) {
                    switch (cc->s480p_mode) {
                        case 0: // Auto
                        case 2: // VESA 640x480@60
                            break;
                        default:
                            continue;
                    }
                }
                break;
            default:
                break;
        }

        target_lm = valid_lm[*group_ptr[mode_preset->group]];

        if ((target_lm & mode_preset->flags) &&
            (vm_in->timings.interlaced == mode_preset->timings.interlaced))
        {
            diff_lines = abs(vm_in->timings.v_total - mode_preset->timings.v_total);
            diff_v_hz_x100 = abs(vm_in->timings.v_hz_x100 - mode_preset->timings.v_hz_x100);

            if ((diff_lines < mindiff_lines) || ((mode_preset->group >= GROUP_720P) && (diff_lines == mindiff_lines) && (diff_v_hz_x100 < mindiff_v_hz_x100))) {
                mindiff_id = i;
                mindiff_lines = diff_lines;
                mindiff_v_hz_x100 = diff_v_hz_x100;
                mindiff_lm = target_lm;
            } else if ((mindiff_lines <= 2) && (diff_lines > mindiff_lines)) {
                // Break out if suitable mode already found
                break;
            }
        }
    }

    if (mindiff_lines >= 130)
        return -1;

    mode_preset = &video_modes_plm[mindiff_id];

    vm_in->timings.h_active = mode_preset->timings.h_active;
    vm_in->timings.v_active = mode_preset->timings.v_active;
    vm_in->timings.h_synclen = mode_preset->timings.h_synclen;
    vm_in->timings.v_synclen = mode_preset->timings.v_synclen;
    vm_in->timings.h_backporch = mode_preset->timings.h_backporch;
    vm_in->timings.v_backporch = mode_preset->timings.v_backporch;
    vm_in->timings.h_total = mode_preset->timings.h_total;
    vm_in->timings.h_total_adj = mode_preset->timings.h_total_adj;
    vm_in->sampler_phase = mode_preset->sampler_phase;
    vm_in->mask.h = mode_preset->mask.h;
    vm_in->mask.v = mode_preset->mask.v;
    vm_in->type = mode_preset->type;
    vm_in->group = mode_preset->group;
    vm_in->vic = mode_preset->vic;
    strncpy(vm_in->name, mode_preset->name, 10);

    memcpy(vm_out, vm_in, sizeof(mode_data_t));
    vm_out->vic = HDMI_Unknown;

    memset(vm_conf, 0, sizeof(vm_proc_config_t));

    {
        const ds_mode_t *ds = find_ds_mode(cc, mode_preset, vm_in);
        if (ds && (ds->out_h_total <= DS_OUT_H_TOTAL_MAX)) {
            if (setup_ds_mode(ds, cc, vm_in, vm_out, vm_conf) != 0)
                return -1;
            return mindiff_id;
        }
    }

    mindiff_lm &= mode_preset->flags;    //ensure L2 mode uniqueness

    // Set passthru flag for all passthru mode options
    if (*group_ptr[mode_preset->group] == 0)
        mindiff_lm = MODE_PT;

    if (mindiff_lm >= MODE_L6_GEN_4_3)
        vm_conf->y_rpt = 5;
    else if (mindiff_lm >= MODE_L5_GEN_4_3)
        vm_conf->y_rpt = 4;
    else if (mindiff_lm >= MODE_L4_GEN_4_3)
        vm_conf->y_rpt = 3;
    else if (mindiff_lm >= MODE_L3_GEN_16_9)
        vm_conf->y_rpt = 2;
    else if (mindiff_lm >= MODE_L2)
        vm_conf->y_rpt = 1;

    switch (mindiff_lm) {
        case MODE_PT:
            vm_out->vic = vm_in->vic;

            if (0) {
                vm_conf->x_rpt = vm_conf->h_skip = 3;
                x_rpt_decr += 1;
            } else if ((cc->pt_mode == 1) && ((mode_preset->group >= GROUP_384P) && (mode_preset->group <= GROUP_576P))) {
                vmode_hv_mult(vm_in, 2, 1);
                vmode_hv_mult(vm_out, 2, 1);
            } else if ((cc->pt_mode >= 2) && (mode_preset->group >= GROUP_240P) && (mode_preset->group <= GROUP_288P)) {
                vm_conf->x_rpt = vm_conf->h_skip = cc->pt_mode;
            }

            // multiply horizontal resolution if necessary to fulfill min. 25MHz TMDS clock requirement. Tweak infoframe pixel repetition indicator later to make sink treat it as original resolution.
            while ((((vm_out->timings.v_hz_x100*vm_out->timings.v_total)/100)*vm_out->timings.h_total*(vm_conf->h_skip+1))>>vm_out->timings.interlaced < 25000000UL) {
                vm_conf->x_rpt = vm_conf->h_skip = 2*(vm_conf->h_skip+1)-1;
            }
            break;
        case MODE_L2:
            // Upsample / pixel-repeat horizontal resolution of 384p/480p/960i modes
            if ((mode_preset->group == GROUP_384P) || (mode_preset->group == GROUP_480P) || (mode_preset->group == GROUP_576P) || ((mode_preset->group == GROUP_1080I) && (mode_preset->timings.h_total < 1200))) {
                if (upsample2x) {
                    vmode_hv_mult(vm_in, 2, 1);
                    vmode_hv_mult(vm_out, 2, VM_OUT_YMULT);
                    skip_hv_mult = 1;
                } else {
                    vm_conf->x_rpt = 1;
                }
            } else {
                if (cc->o480p_pbox) {
                    vm_conf->x_rpt = vm_conf->h_skip = 3;
                    x_rpt_decr += 1;
                }
            }
            break;
        case MODE_L3_GEN_16_9:
            // Upsample / pixel-repeat horizontal resolution of 480i mode
            if ((mode_preset->group == GROUP_480I) || (mode_preset->group == GROUP_576I)) {
                if (upsample2x) {
                    vmode_hv_mult(vm_in, 2, 1);
                    vmode_hv_mult(vm_out, 2, VM_OUT_YMULT);
                    skip_hv_mult = 1;
                } else {
                    vm_conf->x_rpt = 1;
                }
            } else if (cc->hdmi_pr2x_disable && ((mode_preset->group == GROUP_480P) || (mode_preset->group == GROUP_576P))) {
                vm_conf->x_rpt = 1;
            }
            break;
        case MODE_L3_GEN_4_3:
            // Upsample by 3x on GBI Line3x generic
            if (mode_preset->group == GROUP_384P) {
                vmode_hv_mult(vm_in, 3, 1);
                vmode_hv_mult(vm_out, 3, 1);
            }
            vm_conf->x_size = vm_out->timings.h_active-2*vm_in->mask.h;
            vm_out->timings.h_synclen /= 3;
            vm_out->timings.h_backporch /= 3;
            vm_out->timings.h_active /= 3;
            vm_out->timings.h_total /= 3;
            vm_out->timings.h_total_adj = 0;
            vmode_hv_mult(vm_out, 4, VM_OUT_YMULT);
            skip_hv_mult = 1;
            vm_conf->si_pclk_mult = 4;
            break;
        case MODE_L4_GEN_4_3:
            // Upsample / pixel-repeat horizontal resolution of 480i mode
            if ((mode_preset->group == GROUP_480I) || (mode_preset->group == GROUP_576I)) {
                if (upsample2x) {
                    vmode_hv_mult(vm_in, 2, 1);
                    vmode_hv_mult(vm_out, 2, VM_OUT_YMULT);
                    skip_hv_mult = 1;
                } else {
                    vm_conf->x_rpt = 1;
                }
            }
            break;
        case MODE_L5_GEN_4_3:
            if (cc->hdmi_pr2x_disable && mode_preset->group == GROUP_288P) {
                vmode_hv_mult(vm_in, 2, 1);
                vmode_hv_mult(vm_out, 2, VM_OUT_YMULT);
                skip_hv_mult = 1;
            }
            break;
        case MODE_L6_GEN_4_3:
            if (cc->hdmi_pr2x_disable) {
                if (mode_preset->group == GROUP_288P) {
                    // upsampled active would exceed 2048, must pixel-repeat
                    vm_conf->x_rpt = 1;
                } else {
                    vmode_hv_mult(vm_in, 2, 1);
                    vmode_hv_mult(vm_out, 2, VM_OUT_YMULT);
                    skip_hv_mult = 1;
                }
            }
            break;
        case MODE_L2_512_COL:
        case MODE_L2_384_COL:
        case MODE_L2_320_COL:
            if (cc->o480p_pbox) {
                vm_conf->x_rpt = vm_conf->h_skip = 3;
                x_rpt_decr += 1;
            } else {
                vm_conf->x_rpt = vm_conf->h_skip = 1;
            }
            break;
        case MODE_L3_512_COL:
        case MODE_L4_512_COL:
            vm_conf->x_rpt = vm_conf->h_skip = 1;
            break;
        case MODE_L6_512_COL:
            vm_conf->x_rpt = vm_conf->h_skip = cc->hdmi_pr2x_disable ? 3 : 1;
            break;
        case MODE_L2_256_COL:
            if (cc->o480p_pbox) {
                vm_conf->x_rpt = vm_conf->h_skip = 7;
                x_rpt_decr += 2;
            } else {
                vm_conf->x_rpt = vm_conf->h_skip = 2;
            }
            break;
        case MODE_L3_384_COL:
        case MODE_L4_384_COL:
        case MODE_L5_512_COL:
            vm_conf->x_rpt = vm_conf->h_skip = 2;
            break;
        case MODE_L6_384_COL:
            vm_conf->x_rpt = vm_conf->h_skip = cc->hdmi_pr2x_disable ? 4 : 2;
            break;
        case MODE_L6_320_COL:
            vm_conf->x_rpt = vm_conf->h_skip = cc->hdmi_pr2x_disable ? 5 : 2;
            break;
        case MODE_L3_240x360:
            vm_conf->x_rpt = vm_conf->h_skip = 7;
            break;
        case MODE_L3_320_COL:
        case MODE_L4_320_COL:
        case MODE_L5_384_COL:
            vm_conf->x_rpt = vm_conf->h_skip = 3;
            break;
        case MODE_L6_256_COL:
            vm_conf->x_rpt = vm_conf->h_skip = cc->hdmi_pr2x_disable ? 7 : 3;
            break;
        case MODE_L2_240x360:
        case MODE_L3_256_COL:
        case MODE_L4_256_COL:
        case MODE_L5_320_COL:
            vm_conf->x_rpt = vm_conf->h_skip = 4;
            break;
        case MODE_L5_256_COL:
            vm_conf->x_rpt = vm_conf->h_skip = 5;
            break;
        default:
            printf("WARNING: invalid mindiff_lm\n");
            return -1;
            break;
    }

    if (!skip_hv_mult)
        vmode_hv_mult(vm_out, VM_OUT_XMULT, VM_OUT_YMULT);

    // Set clock multiplication factor if not done earlier
    if (!vm_conf->si_pclk_mult)
        vm_conf->si_pclk_mult = VM_OUT_PCLKMULT;

    // Reduce x_rpt for 1:1 PAR 256col mode
    if (mindiff_lm & (MODE_L2_256_COL|MODE_L4_256_COL|MODE_L5_256_COL))
        vm_conf->x_rpt -= cc->ar_256col;
    else if (mindiff_lm & MODE_L3_256_COL)
        vm_conf->x_rpt = cc->ar_256col ? 2 : 3;
    else if (mindiff_lm & MODE_L6_256_COL)
        vm_conf->x_rpt = cc->ar_256col ? (cc->hdmi_pr2x_disable+1)*3-1 : (cc->hdmi_pr2x_disable+1)*4-1;

    if (mindiff_lm & (MODE_L3_320_COL|MODE_L2_240x360))
        x_rpt_decr += 1;
    else if (mindiff_lm & (MODE_L3_240x360))
        x_rpt_decr += 2;

    vm_conf->x_rpt -= x_rpt_decr;

    if (mindiff_lm == MODE_L2_240x360) {
        vm_out->timings.h_active += 80;
        vm_out->timings.h_backporch -= 40;
    }

    // Force TX pixel-repeat for high bandwidth modes
    if (!cc->hdmi_pr2x_disable && (((mindiff_lm == MODE_L5_GEN_4_3) && (mode_preset->group == GROUP_288P)) ||
       ((mindiff_lm == MODE_L3_GEN_16_9) && ((mode_preset->group == GROUP_480P) || (mode_preset->group == GROUP_576P))) ||
       (mindiff_lm >= MODE_L6_GEN_4_3)))
        vm_conf->tx_pixelrep = 1;

    sniprintf(vm_out->name, 11, "%s x%u", vm_in->name, vm_conf->y_rpt+1);

    if (vm_conf->x_size == 0)
        vm_conf->x_size = (vm_in->timings.h_active-2*vm_in->mask.h)*(vm_conf->x_rpt+1);
    if (vm_conf->y_size == 0)
        vm_conf->y_size = vm_out->timings.v_active-2*vm_in->mask.v*(vm_conf->y_rpt+1);

    vm_conf->x_offset = ((vm_out->timings.h_active-vm_conf->x_size)/2);
    vm_conf->x_start_lb = vm_in->mask.h;
    vm_conf->y_offset = ((vm_out->timings.v_active-vm_conf->y_size)/2);

    // Line5x format
    if ((vm_conf->y_rpt == 4) && !((mindiff_lm == MODE_L5_GEN_4_3) && (mode_preset->group == GROUP_288P))) {
        // adjust output width to 1920
        if (cc->l5_fmt != 1) {
            vm_conf->x_offset = (1920-vm_conf->x_size)/2;
            vm_out->timings.h_synclen = (vm_out->timings.h_total - 1920)/4;
            vm_out->timings.h_backporch = (vm_out->timings.h_total - 1920)/2;
            vm_out->timings.h_active = 1920;
        }

        // adjust output height to 1080
        if (cc->l5_fmt == 0) {
            vm_conf->y_start_lb = (vm_out->timings.v_active-1080)/10;
            vm_out->timings.v_backporch += 5*vm_conf->y_start_lb;
            vm_out->timings.v_active = 1080;
            vm_conf->y_size = vm_out->timings.v_active-2*vm_in->mask.v*(vm_conf->y_rpt+1);
        }
    }

    // Aspect
    if (vm_out->type & VIDEO_HDTV) {
        vm_out->ar.h = 16;
        vm_out->ar.v = 9;
    } else {
        vm_out->ar.h = 4;
        vm_out->ar.v = 3;
    }

#ifdef LM_EMIF_EXTRA_DELAY
    vm_conf->framesync_line = ((vm_out->timings.v_total>>vm_out->timings.interlaced)-(1+vm_out->timings.interlaced)*(vm_conf->y_rpt+1));
#else
    //vm_conf->framesync_line = vm_in->timings.interlaced ? ((vm_out->timings.v_total>>vm_out->timings.interlaced)-(vm_conf->y_rpt+1)) : 0;
    vm_conf->framesync_line = 0;
#endif
    vm_conf->framelock = 1;

    // 480i/576i input shown as 240p/288p: the output keeps the interlaced
    // line timing (fields of 263 and 262 lines, frame locked like the
    // passthrough) but both vsyncs start at a line start, so that both fields
    // land on the same scanlines.
    if ((mindiff_lm == MODE_PT) && vm_in->timings.interlaced && cc->ds_i2p &&
        ((mode_preset->group == GROUP_480I) || (mode_preset->group == GROUP_576I)))
        vm_conf->ds_i2p = 1;

    if (mindiff_lm == MODE_PT)
        ds_pt_position(cc, vm_out, vm_conf);

    if (vm_out->vic == HDMI_Unknown)
        vm_out->vic = cc->default_vic;

    return mindiff_id;
}

/*
 * Internal test pattern / "safe screen": 1440x240 @ 27MHz (1716x262),
 * 15.73kHz / 60Hz. Shown at boot, without sync and for unsupported inputs, so
 * the output is always at 15kHz.
 */
void get_testpattern_mode(avconfig_t *cc, mode_data_t *vm_in, mode_data_t *vm_out, vm_proc_config_t *vm_conf)
{
    int i;

    memset(vm_conf, 0, sizeof(vm_proc_config_t));
    memset(vm_in, 0, sizeof(mode_data_t));

    for (i=0; i<num_video_modes_plm; i++) {
        if (video_modes_plm_default[i].vic == HDMI_240p60)
            break;
    }
    memcpy(vm_out, &video_modes_plm_default[i], sizeof(mode_data_t));
    vmode_hv_mult(vm_out, 2, 1);    // 858 -> 1716 clocks at 27MHz
    vm_out->ar.h = 4;
    vm_out->ar.v = 3;
    vm_conf->ds_csync = cc->ds_csync;
}

int get_vmode(vmode_t vmode_id, mode_data_t *vm_in, mode_data_t *vm_out, vm_proc_config_t *vm_conf)
{
    memset(vm_conf, 0, sizeof(vm_proc_config_t));
    memset(vm_in, 0, sizeof(mode_data_t));
    memcpy(vm_out, &video_modes_plm_default[vmode_id], sizeof(mode_data_t));
    vm_out->ar.h = 4;
    vm_out->ar.v = 3;

    return 0;
}
