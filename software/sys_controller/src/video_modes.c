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
    uint8_t vdec;               // stored line = every (vdec+1)th input line
    uint8_t x_rpt;              // each sample lasts (x_rpt+1) output clocks
    uint8_t lb_layout;          // 1 = 8x1024, 2 = 16x512
    uint8_t out_v_backporch;    // output active starts at line 3+out_v_backporch
    uint8_t v_start;            // first line of the first group (before "Line phase")
    uint16_t in_crop;           // source pixels cropped on each side
    uint8_t ar43;               // 720p "Center 4:3" variant
    uint8_t hfilt;              // ADC samples (hfilt+1) per stored sample, averaged
    uint8_t ilace;              // 480i output: one field per input frame
    uint8_t fB_st, fB_n;        // 480i field B: line in group / lines averaged
    char name[11];
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
    { HDMI_480p60,      525, 1048, 2100, 262, 1, 1, 1, 17, 0,   0, 0, 0, 0, 0, 0, "480p>240p" },
    //   (no filtered 240p variant: 2x oversampling would need >4095 clocks/line)
    // 640x480 VGA: same timing, odd lines
    { HDMI_640x480p60,  525, 1048, 2100, 262, 1, 1, 1, 17, 0,   0, 0, 0, 0, 0, 0, "VGA>240p" },

    /* ---- 480i output (525 lines, 2 fields of 262.5) ----
     * Each input frame becomes one field: field A (upper) takes line 0 of every
     * group, field B line 1 (480p) or the average of lines 1-2 (720p: rows at
     * 1.5-line spacing). Frame lock: ADC * in_v_total * 2 == out_h * 525. */
    // 720p: 658x750 -> 1880 (2.857 input lines per output line)
    { HDMI_720p60,      750,  658, 1880, 525, 2, 2, 2, 19, 0,   0, 0, 0, 1, 2, 2, "720p>480i" },
    { HDMI_720p60,      750,  658, 3760, 525, 2, 5, 2, 19, 0,   0, 0, 1, 1, 2, 2, "720p>480i" },
    { HDMI_720p60,      750,  784, 2240, 525, 2, 3, 2, 19, 0, 160, 1, 0, 1, 2, 2, "4:3>480i" },
    { HDMI_720p60,      750,  658, 3760, 525, 2, 7, 2, 19, 0, 160, 1, 1, 1, 2, 2, "4:3>480i" },
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

static uint16_t ds_framesync_line, ds_v_active;

static const ds_mode_t* find_ds_mode(avconfig_t *cc, mode_data_t *preset, mode_data_t *vm_in) {
    unsigned i;
    const ds_mode_t *match = NULL;
    uint8_t want_hfilt = !cc->ds_hfilt_off;
    // wanted output: 0 = 240p, 1 = 480i
    uint8_t want[2];
    unsigned pass, npass = 0;

    if (vm_in->timings.interlaced || (vm_in->timings.v_hz_x100 < 5800) || (vm_in->timings.v_hz_x100 > 6200))
        return NULL;

    if (cc->ds_480i)
        want[npass++] = 1;
    want[npass++] = 0;

    // pick the variant with the requested output and filter setting; fall
    // back to another filter setting, then to the next output type
    for (pass=0; pass<npass; pass++) {
        for (i=0; i<sizeof(ds_modes)/sizeof(ds_mode_t); i++) {
            if ((ds_modes[i].vic == preset->vic) &&
                (ds_modes[i].ilace == want[pass]) &&
                ((ds_modes[i].vic != HDMI_720p60) || (ds_modes[i].ar43 == !!cc->ds_720p_ar))) {
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

#define DS_OUT_H_TOTAL_MAX  4095

static void setup_ds_mode(const ds_mode_t *m, avconfig_t *cc, mode_data_t *vm_in, mode_data_t *vm_out, vm_proc_config_t *vm_conf) {
    uint16_t in_h_total = vm_in->timings.h_total;
    uint16_t smp_active, out_h_synclen, out_h_bp_min, out_h_fp_min, out_active_max, out_active, rpt;

    // Optional source crop (e.g. center 4:3 of 16:9): narrower capture window
    vm_in->timings.h_backporch += m->in_crop;
    vm_in->timings.h_active -= 2*m->in_crop;

    // Resample horizontal timings to smp_h_total samples per line
    smp_active = (vm_in->timings.h_active*m->smp_h_total + in_h_total/2)/in_h_total;
    vm_in->timings.h_synclen = (vm_in->timings.h_synclen*m->smp_h_total + in_h_total/2)/in_h_total;
    vm_in->timings.h_backporch = (vm_in->timings.h_backporch*m->smp_h_total + in_h_total/2)/in_h_total;
    vm_in->timings.h_active = smp_active;
    vm_in->timings.h_total = m->smp_h_total;
    vm_in->timings.h_total_adj = 0;

    memcpy(vm_out, vm_in, sizeof(mode_data_t));

    // 15kHz horizontal timings, in fractions of the 63.6us line:
    // hsync 4.7us, back porch >= 5.15us, front porch >= 1.4us
    rpt = m->x_rpt+1;
    out_h_synclen = (m->out_h_total*74 + 500)/1000;   // 9-bit field (up to 511)
    out_h_bp_min = (m->out_h_total*81 + 500)/1000;
    out_h_fp_min = (m->out_h_total*22 + 500)/1000;
    out_active_max = ((m->out_h_total - out_h_synclen - out_h_bp_min - out_h_fp_min)/rpt)*rpt;
    out_active = smp_active*rpt;
    if (out_active > out_active_max)
        out_active = out_active_max;    // crop the sides evenly

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
        if (bp > 511)
            bp = 511;           // 9-bit register field
        vm_out->timings.h_backporch = bp;
    }
    vm_out->timings.h_total_adj = 0;
    vm_out->timings.v_total = m->out_v_total;       // 525 for 480i (per frame)
    vm_out->timings.v_active = DS_OUT_V_ACTIVE;     // per field
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
        int field_lines = m->ilace ? (m->out_v_total/2) : m->out_v_total;
        int vbp_max = field_lines - DS_OUT_V_FPORCH - DS_OUT_V_SYNCLEN - DS_OUT_V_ACTIVE;
        int vbp = ((m->out_v_backporch > vbp_max) ? vbp_max : m->out_v_backporch) + ((int)cc->ds_vpos - SIGNED_NUMVAL_ZERO);
        int v_active = DS_OUT_V_ACTIVE;
        int delay;

        if (vbp < DS_OUT_V_BPORCH_MIN)
            vbp = DS_OUT_V_BPORCH_MIN;
        if (vbp > vbp_max)
            v_active -= vbp - vbp_max;
        delay = (int)m->out_v_backporch - vbp;   // output field start after the input frame start, in lines

        vm_out->timings.v_backporch = vbp;
        vm_out->timings.v_active = v_active;
        ds_v_active = v_active;
        ds_framesync_line = (delay > 0) ? (field_lines - delay) : -delay;
    }
    vm_out->timings.interlaced = !!m->ilace;
    vm_out->type = VIDEO_SDTV;
    vm_out->ar.h = 4;
    vm_out->ar.v = 3;
    vm_out->vic = HDMI_Unknown;

    vm_conf->x_rpt = m->x_rpt;
    vm_conf->y_rpt = 0;
    vm_conf->h_skip = m->hfilt;         // ADC samples (hfilt+1) per stored sample
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
    if (m->ilace) {
        vm_conf->ds_field_cfg = (0 | (1<<2)) | ((m->fB_st | (m->fB_n<<2)) << 4);
    } else if (vm_conf->ds_vblend) {
        // average the whole group
        vm_conf->ds_field_cfg = m->vdec | ((m->vdec+1)<<2);
    } else {
        // pick the middle line of the group (720p: line 1 of 3, 480p: line 0 of 2)
        vm_conf->ds_field_cfg = (m->vdec/2) | (1<<2);
    }
    vm_conf->ds_hfilt = m->hfilt;
    vm_conf->ds_vdec = m->vdec;
    vm_conf->ds_lb_layout = m->lb_layout;
    vm_conf->ds_csync = cc->ds_csync;

    vm_conf->framesync_line = ds_framesync_line;
    vm_conf->framelock = 1;
    vm_conf->si_pclk_mult = 1;
    vm_conf->tx_pixelrep = 0;
    vm_conf->hdmitx_pixr_ifr = 0;

    strncpy(vm_out->name, m->name, 11);

    if (vm_out->vic == HDMI_Unknown)
        vm_out->vic = cc->default_vic;
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
            setup_ds_mode(ds, cc, vm_in, vm_out, vm_conf);
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
