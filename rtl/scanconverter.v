//
// Copyright (C) 2019-2023  Markus Hiienkari <mhiienka@niksula.hut.fi>
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

`include "lat_tester_includes.v"

(* multstyle = "dsp" *)
module scanconverter (
    input PCLK_CAP_i,
    input PCLK_OUT_i,
    input reset_n,
    input [7:0] R_i,
    input [7:0] G_i,
    input [7:0] B_i,
    input HSYNC_i,
    input VSYNC_i,
    input DE_i,
    input FID_i,
    input datavalid_i,
    input interlaced_in_i,
    input frame_change_i,
    input [10:0] xpos_i,
    input [10:0] ypos_i,
    input [11:0] h_in_active,
    input [31:0] hv_out_config,
    input [31:0] hv_out_config2,
    input [31:0] hv_out_config3,
    input [31:0] xy_out_config,
    input [31:0] xy_out_config2,
    // Downscaler firmware: extension bits for wide horizontal timings
    // [0] h_total[12] [1] h_active[12] [2] h_synclen[8] [3] x_size[12] [4] x_rpt[4] [5] h_backporch[9]
    input [31:0] hv_ext_config,
    input [31:0] misc_config,
    input [31:0] sl_config,
    input [31:0] sl_config2,
    input [31:0] sl_config3,
    input testpattern_enable,
    input lb_enable,
    input [2:0] ds_vdec,
    input [1:0] lb_layout,
    input [1:0] ds_hfilt,
    input [9:0] ds_field_cfg,
    input ds_ilace,
    input ds_fswap,             // 480i: put field B (lower rows) in the odd field
    input ds_i2p,               // interlaced output timing with both vsyncs at a line start (480i -> 240p)
    input ds_vflt,              // 480p -> 480i flicker filter (see linebuf_top)
    input ds_vflt_bofs,
    input [4:0] ds_vflt_a,
    input [9:0] lb_stride,      // line buffer layout 3: words per line
    input [5:0] lb_slots,       // line buffer layout 3: number of lines
    input lb_rgb,               // line buffer layout 3: RGB, one pixel per word
    input [12:0] ds_hofs,       // output position at the input frame start (clocks, 0 = line start)
    input ext_sync_mode,
    input ext_frame_change_i,
    input [7:0] ext_R_i,
    input [7:0] ext_G_i,
    input [7:0] ext_B_i,
    output PCLK_o,
    output [7:0] R_o,
    output [7:0] G_o,
    output [7:0] B_o,
    output HSYNC_o,
    output CSYNC_o,             // standard composite sync (equalizing + serrated vsync)
    output VSYNC_o,
    output DE_o,
    output [11:0] xpos_o,
    output [10:0] ypos_o,
    output reg [3:0] x_ctr_shmask,
    output reg [3:0] y_ctr_shmask,
    input [11:0] shmask_data,
    output reg resync_strobe,
    // Downscaler diagnostics (read by the CPU)
    output reg [7:0] ds_resync_cnt,
    output reg [9:0] ds_fperiod_lo,
    input lt_active,
    input [1:0] lt_mode,
    input emif_br_clk,
    input emif_br_reset,
    output [27:0] emif_rd_addr,
    output emif_rd_read,
    input [255:0] emif_rd_rdata,
    input emif_rd_waitrequest,
    input emif_rd_readdatavalid,
    output [5:0] emif_rd_burstcount,
    output [27:0] emif_wr_addr,
    output emif_wr_write,
    output [255:0] emif_wr_wdata,
    input emif_wr_waitrequest,
    output [5:0] emif_wr_burstcount
);

parameter EMIF_ENABLE = 0;
parameter NUM_LINE_BUFFERS = 32;


localparam FID_EVEN = 1'b0;
localparam FID_ODD = 1'b1;

localparam PP_PL_START          = 1;
localparam PP_MASK_START        = PP_PL_START;
localparam PP_MASK_LENGTH       = 1;
localparam PP_MASK_END          = PP_MASK_START + PP_MASK_LENGTH;
localparam PP_LINEBUF_START     = PP_MASK_END;
localparam PP_LINEBUF_LENGTH    = 4;     // RAM + output register + YCbCr -> RGB (see linebuf_top)
localparam PP_LINEBUF_END       = PP_LINEBUF_START + PP_LINEBUF_LENGTH;
localparam PP_SRCSEL_START      = PP_LINEBUF_END;
localparam PP_SRCSEL_LENGTH     = 1;
localparam PP_SRCSEL_END        = PP_SRCSEL_START + PP_SRCSEL_LENGTH;
localparam PP_SHMASK_START      = PP_SRCSEL_START;
localparam PP_SHMASK_LENGTH     = 3;
localparam PP_SHMASK_END        = PP_SHMASK_START + PP_SHMASK_LENGTH;
localparam PP_Y_CALC_START      = PP_SRCSEL_END;
localparam PP_Y_CALC_LENGTH     = 2;
localparam PP_Y_CALC_END        = PP_Y_CALC_START + PP_Y_CALC_LENGTH;
localparam PP_SLGEN_START       = PP_Y_CALC_END;
localparam PP_SLGEN_LENGTH      = 5;
localparam PP_SLGEN_END         = PP_SLGEN_START + PP_SLGEN_LENGTH;
localparam PP_TP_START          = PP_SLGEN_END;
localparam PP_TP_LENGTH         = 1;
localparam PP_TP_END            = PP_TP_START + PP_TP_LENGTH;
localparam PP_PL_END            = PP_TP_END;

wire [12:0] H_TOTAL = {hv_ext_config[0], hv_out_config[11:0]};
wire [12:0] H_ACTIVE = {hv_ext_config[1], hv_out_config[23:12]};
wire [8:0] H_SYNCLEN = {hv_ext_config[2], hv_out_config[31:24]};
wire [9:0] H_BACKPORCH = {hv_ext_config[5], hv_out_config2[8:0]};

wire V_INTERLACED = hv_out_config2[31];
wire [10:0] V_TOTAL = hv_out_config2[19:9] >> V_INTERLACED;
wire [10:0] V_ACTIVE = hv_out_config2[30:20];
wire [3:0] V_SYNCLEN = hv_out_config3[3:0];
wire [8:0] V_BACKPORCH = hv_out_config3[12:4];

wire [10:0] V_STARTLINE = hv_out_config3[23:13];

wire [10:0] V_STARTLINE_PREV = (V_STARTLINE == 0) ? (V_TOTAL-1) : (V_STARTLINE-1);
// output field parity at V_STARTLINE and at the line before it
wire FID_START = (V_STARTLINE < (V_TOTAL/2)) ? FID_ODD : FID_EVEN;
wire FID_PREV = (V_STARTLINE == 0) ? FID_EVEN : FID_START;

wire [12:0] X_SIZE = {hv_ext_config[3], xy_out_config[11:0]};
wire [10:0] Y_SIZE = xy_out_config[22:12];
wire signed [9:0] X_OFFSET = xy_out_config2[9:0];
wire signed [8:0] Y_OFFSET = xy_out_config[31:23];

wire [7:0] X_START_LB = xy_out_config2[17:10];
// In downscaler mode the y_start_lb field holds the first stored input line
// (see linebuf_top) and the output always starts from line buffer 0.
wire [5:0] DS_VSTART = xy_out_config2[23:18];
wire signed [5:0] Y_START_LB = (ds_vdec != 3'h0) ? 6'sd0 : xy_out_config2[23:18];

wire signed [4:0] X_RPT = {hv_ext_config[4], xy_out_config2[27:24]};
wire signed [3:0] Y_RPT = xy_out_config2[31:28];

wire Y_SKIP = (Y_RPT == 4'(-1));
wire [1:0] Y_STEP = Y_SKIP+1'b1;

wire [3:0] SL_L_STR[5:0] = '{sl_config[23:20], sl_config[19:16], sl_config[15:12], sl_config[11:8], sl_config[7:4], sl_config[3:0]};
wire [3:0] SL_C_STR[9:0] = '{sl_config3[7:4], sl_config3[3:0], sl_config2[31:28], sl_config2[27:24], sl_config2[23:20], sl_config2[19:16], sl_config2[15:12], sl_config2[11:8], sl_config2[7:4], sl_config2[3:0]};
wire [5:0] SL_L_OVERLAY = sl_config[29:24];
wire SL_METHOD_PRE = sl_config[30];
wire SL_BOB_ALTERN = sl_config[31];
wire [9:0] SL_C_OVERLAY = sl_config3[17:8];
wire [2:0] SL_IV_Y = sl_config3[20:18];
wire [3:0] SL_IV_X = sl_config3[24:21];
wire [4:0] SL_HYBRSTR = sl_config3[29:25];

wire [3:0] MISC_MASK_BR = misc_config[3:0];
wire [2:0] MISC_MASK_COLOR = misc_config[6:4];
wire MISC_LM_DEINT_MODE = 1'b0;
wire MISC_NIR_EVEN_OFFSET = 1'b0;
wire [3:0] MISC_BFI_STR = 4'h0;
wire MISC_BFI_ENABLE = 1'b0;
wire MISC_SHMASK_ENABLE = misc_config[15];
wire [3:0] MISC_SHMASK_IV_X = misc_config[19:16];
wire [3:0] MISC_SHMASK_IV_Y = misc_config[23:20];
wire MISC_PANASONIC_HACK = misc_config[24];

wire [7:0] MASK_R = MISC_MASK_COLOR[2] ? {2{MISC_MASK_BR}} : 8'h00;
wire [7:0] MASK_G = MISC_MASK_COLOR[1] ? {2{MISC_MASK_BR}} : 8'h00;
wire [7:0] MASK_B = MISC_MASK_COLOR[0] ? {2{MISC_MASK_BR}} : 8'h00;

// Latency tester box bounds
wire [11:0] LT_BOX_X_START = (lt_mode == `LT_POS_TOPLEFT) ? 0 : ((lt_mode == `LT_POS_CENTER) ? ((H_ACTIVE/2'h2)-(H_ACTIVE/(`LT_WIDTH_DIV*2'h2))) : (H_ACTIVE-(H_ACTIVE/`LT_WIDTH_DIV)));
wire [11:0] LT_BOX_X_STOP  = (lt_mode == `LT_POS_TOPLEFT) ? (H_ACTIVE/`LT_WIDTH_DIV) : ((lt_mode == `LT_POS_CENTER) ? ((H_ACTIVE/2'h2)+(H_ACTIVE/(`LT_WIDTH_DIV*2'h2))) : H_ACTIVE);
wire [11:0] LT_BOX_Y_START = (lt_mode == `LT_POS_TOPLEFT) ? 0 : ((lt_mode == `LT_POS_CENTER) ? ((V_ACTIVE/2'h2)-(V_ACTIVE/(`LT_HEIGHT_DIV*2'h2))) : (V_ACTIVE-(V_ACTIVE/`LT_HEIGHT_DIV)));
wire [11:0] LT_BOX_Y_STOP  = (lt_mode == `LT_POS_TOPLEFT) ? (V_ACTIVE/`LT_HEIGHT_DIV) : ((lt_mode == `LT_POS_CENTER) ? ((V_ACTIVE/2'h2)+(V_ACTIVE/(`LT_HEIGHT_DIV*2'h2))) : V_ACTIVE);


reg frame_change_sync1_reg, frame_change_sync2_reg, frame_change_prev, frame_change_resync;
// Downscaler 480i: every input frame becomes one output field. in_field is
// the parity of the current input frame; the output is (re)aligned only on
// field A frames, so that field A always ends up in the ODD output field.
reg in_field;
// line buffer layout 3: number of lines and base address of the line being read
wire [10:0] LB_NUM = (lb_layout == 2'h3) ? {5'h0, lb_slots} : NUM_LINE_BUFFERS;
reg [12:0] lb_rd_base, lb_rd_base_next;
always @(posedge PCLK_OUT_i)
    lb_rd_base_next <= ypos_lb_next[5:0] * lb_stride;
reg [19:0] fp_ctr;
wire frame_change = frame_change_sync2_reg;

reg [12:0] h_cnt;
reg [10:0] v_cnt;
reg h_avidstart, v_avidstart;
reg src_fid, dst_fid;
wire [6:0] panasonic_adjust = (MISC_PANASONIC_HACK & (Y_RPT == 4'h1) & (v_cnt == V_SYNCLEN+V_BACKPORCH+V_ACTIVE-1)) ? 8'd128 : 7'd0;

reg [10:0] xpos_lb;
wire [10:0] xpos_lb_start = (X_OFFSET < 10'sd0) ? 11'd0 : {1'b0, X_OFFSET};
reg [10:0] ypos_lb, ypos_lb_next;
reg [4:0] x_ctr;
reg [3:0] y_ctr;
reg line_id;
reg ypos_pp_init;

reg sl_method;
reg [3:0] sl_str;
reg [7:0] sl_str_thold, Y_sl_str, R_sl_str, G_sl_str, B_sl_str;
wire [7:0] R_sl_mult, G_sl_mult, B_sl_mult;
wire bfi_frame;

reg [8:0] Y_rb_tmp;
reg [9:0] Y;
wire [8:0] Y_sl_hybr_ref_pre, R_sl_hybr_ref_pre, G_sl_hybr_ref_pre, B_sl_hybr_ref_pre;
wire [8:0] Y_sl_hybr_ref, R_sl_hybr_ref, G_sl_hybr_ref, B_sl_hybr_ref;

reg [4:0] R_shmask_str, G_shmask_str, B_shmask_str;
wire [8:0] R_shmask_mult, G_shmask_mult, B_shmask_mult;

wire [7:0] R_vg, G_vg, B_vg;
wire [7:0] R_linebuf, G_linebuf, B_linebuf;

// Pipeline registers
reg [7:0] R_pp[PP_SRCSEL_END:PP_PL_END] /* synthesis ramstyle = "logic" */;
reg [7:0] G_pp[PP_SRCSEL_END:PP_PL_END] /* synthesis ramstyle = "logic" */;
reg [7:0] B_pp[PP_SRCSEL_END:PP_PL_END] /* synthesis ramstyle = "logic" */;
reg HSYNC_pp[PP_PL_START:PP_PL_END] /* synthesis ramstyle = "logic" */;
reg CSYNC_pp[PP_PL_START:PP_PL_END] /* synthesis ramstyle = "logic" */;
reg VSYNC_pp[PP_PL_START:PP_PL_END] /* synthesis ramstyle = "logic" */;
reg DE_pp[PP_PL_START:PP_PL_END] /* synthesis ramstyle = "logic" */;
reg [12:0] xpos_pp[PP_PL_START:PP_PL_START];
reg [10:0] ypos_pp[PP_PL_START:PP_PL_START];
// Instead of carrying the 24-bit position through the whole pipeline, only
// the line/field start events are delayed and the position is regenerated
// with the same counters for the test pattern stage and the output.
reg evh_pp[PP_PL_START:PP_TP_START] /* synthesis ramstyle = "logic" */;
reg evv_pp[PP_PL_START:PP_TP_START] /* synthesis ramstyle = "logic" */;
reg [12:0] xpos_tp, xpos_end;
reg [10:0] ypos_tp, ypos_end;
reg mask_enable_pp[PP_MASK_END:PP_TP_START] /* synthesis ramstyle = "logic" */;
reg lt_box_enable_pp[PP_MASK_END:PP_TP_START] /* synthesis ramstyle = "logic" */;
reg draw_sl_pp[(PP_SLGEN_START+1):(PP_SLGEN_END-1)] /* synthesis ramstyle = "logic" */;
reg [3:0] x_ctr_sl_pp[PP_PL_START:PP_SLGEN_START] /* synthesis ramstyle = "logic" */;
reg [2:0] y_ctr_sl_pp[PP_PL_START:PP_SLGEN_START] /* synthesis ramstyle = "logic" */;

assign PCLK_o = PCLK_OUT_i;


lpm_mult_8x5_9 Y_sl_hybr_ref_pre_u
(
    .clock(PCLK_OUT_i),
    .dataa(Y[9:2]),
    .datab(SL_HYBRSTR),
    .result(Y_sl_hybr_ref_pre)
);
lpm_mult_8x5_9 R_sl_hybr_ref_pre_u
(
    .clock(PCLK_OUT_i),
    .dataa(R_pp[PP_SLGEN_START]),
    .datab(SL_HYBRSTR),
    .result(R_sl_hybr_ref_pre)
);
lpm_mult_8x5_9 G_sl_hybr_ref_pre_u
(
    .clock(PCLK_OUT_i),
    .dataa(G_pp[PP_SLGEN_START]),
    .datab(SL_HYBRSTR),
    .result(G_sl_hybr_ref_pre)
);
lpm_mult_8x5_9 B_sl_hybr_ref_pre_u
(
    .clock(PCLK_OUT_i),
    .dataa(B_pp[PP_SLGEN_START]),
    .datab(SL_HYBRSTR),
    .result(B_sl_hybr_ref_pre)
);

lpm_mult_8x5_9 Y_sl_hybr_ref_u
(
    .clock(PCLK_OUT_i),
    .dataa(Y_sl_hybr_ref_pre[8:1]),
    .datab({sl_str, 1'b0}),
    .result(Y_sl_hybr_ref)
);
lpm_mult_8x5_9 R_sl_hybr_ref_u
(
    .clock(PCLK_OUT_i),
    .dataa(R_sl_hybr_ref_pre[8:1]),
    .datab({sl_str, 1'b0}),
    .result(R_sl_hybr_ref)
);
lpm_mult_8x5_9 G_sl_hybr_ref_u
(
    .clock(PCLK_OUT_i),
    .dataa(G_sl_hybr_ref_pre[8:1]),
    .datab({sl_str, 1'b0}),
    .result(G_sl_hybr_ref)
);
lpm_mult_8x5_9 B_sl_hybr_ref_u
(
    .clock(PCLK_OUT_i),
    .dataa(B_sl_hybr_ref_pre[8:1]),
    .datab({sl_str, 1'b0}),
    .result(B_sl_hybr_ref)
);

lpm_mult_sl R_sl_mult_u
(
    .clock(PCLK_OUT_i),
    .dataa(R_pp[PP_SLGEN_START+3]),
    .datab(~Y_sl_str),
    .result(R_sl_mult)
);
lpm_mult_sl G_sl_mult_u
(
    .clock(PCLK_OUT_i),
    .dataa(G_pp[PP_SLGEN_START+3]),
    .datab(~Y_sl_str),
    .result(G_sl_mult)
);
lpm_mult_sl B_sl_mult_u
(
    .clock(PCLK_OUT_i),
    .dataa(B_pp[PP_SLGEN_START+3]),
    .datab(~Y_sl_str),
    .result(B_sl_mult)
);

lpm_mult_8x5_9 R_shmask_mult_u
(
    .clock(PCLK_OUT_i),
    .dataa(R_pp[PP_SHMASK_START+1]),
    .datab(R_shmask_str),
    .result(R_shmask_mult)
);
lpm_mult_8x5_9 G_shmask_mult_u
(
    .clock(PCLK_OUT_i),
    .dataa(G_pp[PP_SHMASK_START+1]),
    .datab(G_shmask_str),
    .result(G_shmask_mult)
);
lpm_mult_8x5_9 B_shmask_mult_u
(
    .clock(PCLK_OUT_i),
    .dataa(B_pp[PP_SHMASK_START+1]),
    .datab(B_shmask_str),
    .result(B_shmask_mult)
);


linebuf_top #(
    .EMIF_ENABLE(EMIF_ENABLE),
    .NUM_LINE_BUFFERS(NUM_LINE_BUFFERS)
  ) linebuf_top_inst (
    .PCLK_CAP_i(PCLK_CAP_i),
    .PCLK_OUT_i(PCLK_OUT_i),
    .R_i(R_i),
    .G_i(G_i),
    .B_i(B_i),
    .DE_i(DE_i),
    .datavalid_i(datavalid_i),
    .h_in_active(h_in_active),
    .xpos_i(xpos_i),
    .ypos_i(ypos_i),
    .xpos_lb(xpos_lb),
    .ypos_lb(ypos_lb),
    .ypos_lb_next(ypos_lb_next),
    .line_id(line_id),
    .lb_enable(lb_enable),
    .ds_vdec(ds_vdec),
    .ds_vstart(DS_VSTART),
    .lb_layout(lb_layout),
    .ds_hfilt(ds_hfilt),
    .ds_field_cfg(ds_field_cfg),
    .ds_ilace(ds_ilace),
    .vflt(ds_vflt),
    .vflt_bofs(ds_vflt_bofs),
    .vflt_a(ds_vflt_a),
    .in_field(in_field),
    .lb_stride(lb_stride),
    .lb_slots(lb_slots),
    .lb_rgb(lb_rgb),
    .lb_rd_base(lb_rd_base),
    .R_linebuf(R_linebuf),
    .G_linebuf(G_linebuf),
    .B_linebuf(B_linebuf),
    .emif_br_clk(emif_br_clk),
    .emif_br_reset(emif_br_reset),
    .emif_rd_addr(emif_rd_addr),
    .emif_rd_read(emif_rd_read),
    .emif_rd_rdata(emif_rd_rdata),
    .emif_rd_waitrequest(emif_rd_waitrequest),
    .emif_rd_readdatavalid(emif_rd_readdatavalid),
    .emif_rd_burstcount(emif_rd_burstcount),
    .emif_wr_addr(emif_wr_addr),
    .emif_wr_write(emif_wr_write),
    .emif_wr_wdata(emif_wr_wdata),
    .emif_wr_waitrequest(emif_wr_waitrequest),
    .emif_wr_burstcount(emif_wr_burstcount)
);


videogen vg0 (
    .pclk           (PCLK_OUT_i),
    .xpos           (xpos_tp >> 1),                   // 1440x240 test pattern ->
    .ypos           ({ypos_tp[9:0], 1'b0}),           // 720x480 generator coords
    .tp_sel         (misc_config[10:9]),              // test pattern (stock reverse LPF bits)
    .R_out          (R_vg),
    .G_out          (G_vg),
    .B_out          (B_vg)
);


// Frame change strobe synchronization
always @(posedge PCLK_OUT_i) begin
    frame_change_sync1_reg <= frame_change_i;
    frame_change_sync2_reg <= frame_change_sync1_reg;
    frame_change_prev <= frame_change_sync2_reg;

    // aligned = output is at the frame start position (with the right field
    // parity for interlaced output, otherwise e.g. the 480i field order could
    // never change)
    frame_change_resync <= ~frame_change_prev & frame_change &
                           ~(((ds_hofs == 13'h0) & (v_cnt == V_STARTLINE_PREV) & (h_cnt > H_TOTAL-128) & (~V_INTERLACED | (dst_fid == FID_PREV))) |
                             ((ds_hofs == 13'h0) & (v_cnt == V_STARTLINE) & (h_cnt < 128) & (~V_INTERLACED | (dst_fid == FID_START))) |
                             ((ds_hofs != 13'h0) & (v_cnt == V_STARTLINE) & (h_cnt >= ds_hofs-13'd128) & (h_cnt < ds_hofs+13'd128) & (~V_INTERLACED | (dst_fid == FID_START)))) &
                           (~ds_ilace | (in_field ^ ds_fswap));    // 480i: frame shown in the odd field
    if (~frame_change_prev & frame_change)
        in_field <= ds_ilace ? ~in_field : 1'b0;

    // diagnostics: number of output resyncs, input frame period (low bits)
    if (frame_change_resync)
        ds_resync_cnt <= ds_resync_cnt + 1'b1;
    if (~frame_change_prev & frame_change) begin
        ds_fperiod_lo <= fp_ctr[9:0];
        fp_ctr <= 20'd1;
    end else if (fp_ctr != 20'hfffff) begin
        fp_ctr <= fp_ctr + 1'b1;
    end
end

// H/V counters
always @(posedge PCLK_OUT_i) begin
    if (ext_sync_mode & ext_frame_change_i) begin
        h_cnt <= PP_SRCSEL_START; // compensate pipeline delays
        v_cnt <= 0;
        bfi_frame <= bfi_frame ^ 1'b1;
    end else if (~ext_sync_mode & frame_change_resync) begin
        h_cnt <= ds_hofs;
        v_cnt <= V_STARTLINE;
        bfi_frame <= 0;
        src_fid <= (~interlaced_in_i | (V_STARTLINE < (V_TOTAL/2))) ? FID_ODD : FID_EVEN;
        dst_fid <= (~V_INTERLACED | (V_STARTLINE < (V_TOTAL/2))) ? FID_ODD : FID_EVEN;
        resync_strobe <= 1'b1;
    end else begin
        if (h_cnt == H_TOTAL-1) begin
            if ((~V_INTERLACED & (v_cnt == V_TOTAL-1)) |
                (V_INTERLACED & (dst_fid == FID_ODD) & (v_cnt == V_TOTAL)) |
                (V_INTERLACED & (dst_fid == FID_EVEN) & (v_cnt == V_TOTAL-1)))
            begin
                v_cnt <= 0;
                v_avidstart <= (V_SYNCLEN+V_BACKPORCH-1'b1 == 0);
                src_fid <= interlaced_in_i ? (src_fid ^ 1'b1) : FID_ODD;
                dst_fid <= V_INTERLACED ? (dst_fid ^ 1'b1) : FID_ODD;
                resync_strobe <= 1'b0;
            end else begin
                v_cnt <= v_cnt + 1'b1;
                v_avidstart <= (v_cnt == V_SYNCLEN+V_BACKPORCH-2'h2);
            end
            h_cnt <= 0;
            h_avidstart <= 1'b0;
        end else begin
            h_cnt <= h_cnt + 1'b1;
            h_avidstart <= (h_cnt == H_SYNCLEN+H_BACKPORCH-1'b1);
        end
    end
end

// Postprocess pipeline structure
//            1          2         3         4         5         6         7         8         9         10        11        12
// |----------|----------|---------|---------|---------|---------|---------|---------|---------|---------|---------|---------|
// | SYNC/DE  |          |         |         |         |         |         |         |         |         |         |         |
// | X/Y POS  |          |         |         |         |         |         |         |         |         |         |         |
// |          |   MASK   |         |         |         |         |         |         |         |         |         |         |
// |          |  LT_BOX  |         |         |         |         |         |         |         |         |         |         |
// |          | LB_SETUP | LINEBUF |         |         |         |         |         |         |         |         |         |
// |          |          |         | SRCSEL  |         |         |         |         |         |         |         |         |
// |          | SHM_BUF  | SHM_BUF | SHMASK  | SHMASK  | SHMASK  |         |         |         |         |         |         |
// |          |          |         |         |    Y    |    Y    |         |         |         |         |         |         |
// |          |          |         |         |         |         |  SLGEN  |  SLGEN  |  SLGEN  |  SLGEN  |  SLGEN  |         |
// |          |          |         |         |         |         |         |         |         |         |         |    TP   |


// Standard composite sync (as in NTSC): 3 lines of equalizing pulses (half
// width, every half line), 3 lines of serrated vsync (broad pulses every half
// line) and 3 lines of equalizing pulses. Positions are counted in half lines
// from the vsync start of the current field (cs_pcur) or of the next one
// (cs_pnext). In 480i the even field's vsync starts half a line before its
// v_cnt 0, and the odd field (V_TOTAL+1 lines) ends half a line early.
// Computed in 4 register stages (timing), output enters the pipeline at
// stage 4 so that it stays aligned with HSYNC/VSYNC.
reg cs_half_a, cs_even_a, cs_half_b;
reg signed [13:0] cs_flen_a;
reg [12:0] cs_hpos_a;
reg [10:0] cs_v_a;
reg signed [13:0] cs_pcur_b, cs_pnext_b;
reg cs_lt_vs_b, cs_lt_eq_b, cs_lt_hs_b, cs_lt_vs_c, cs_lt_eq_c, cs_lt_hs_c;
reg cs_vs_c, cs_eq_c;
reg cs_d;   // = pipeline stage 4
wire signed [13:0] cs_vs_end = $signed({9'h0, V_SYNCLEN, 1'b0});
wire signed [13:0] cs_p = (cs_pnext_b >= -14'sd6) ? cs_pnext_b : cs_pcur_b;
always @(posedge PCLK_OUT_i) begin
    // stage A (same time as pipeline stage 1)
    cs_half_a <= (h_cnt >= (H_TOTAL/2));
    cs_hpos_a <= (h_cnt >= (H_TOTAL/2)) ? (h_cnt - (H_TOTAL/2)) : h_cnt;
    cs_v_a <= v_cnt;
    cs_even_a <= V_INTERLACED & ~ds_i2p & (dst_fid == FID_EVEN);
    // field length in half lines (480i: 262.5 lines; 480i -> 240p: 263 / 262)
    cs_flen_a <= ds_i2p ? $signed({2'b00, V_TOTAL + {10'h0, (dst_fid == FID_ODD)}, 1'b0}) :
                          ($signed({2'b00, V_TOTAL, 1'b0}) + $signed({13'h0, V_INTERLACED}));
    // stage B
    cs_pcur_b <= $signed({2'b00, cs_v_a, cs_half_a}) + $signed({13'h0, cs_even_a});
    cs_pnext_b <= $signed({2'b00, cs_v_a, cs_half_a}) + $signed({13'h0, cs_even_a}) - cs_flen_a;
    cs_half_b <= cs_half_a;
    cs_lt_vs_b <= (cs_hpos_a < ((H_TOTAL/2) - H_SYNCLEN));
    cs_lt_eq_b <= (cs_hpos_a < (H_SYNCLEN/2));
    cs_lt_hs_b <= ~cs_half_a & (cs_hpos_a < H_SYNCLEN);
    // stage C
    cs_vs_c <= (cs_p >= 0) & (cs_p < cs_vs_end);
    cs_eq_c <= ((cs_p >= -14'sd6) & (cs_p < 0)) | ((cs_p >= cs_vs_end) & (cs_p < cs_vs_end + 14'sd6));
    {cs_lt_vs_c, cs_lt_eq_c, cs_lt_hs_c} <= {cs_lt_vs_b, cs_lt_eq_b, cs_lt_hs_b};
    // stage D (= pipeline stage 4), active low
    cs_d <= ~(cs_vs_c ? cs_lt_vs_c : (cs_eq_c ? cs_lt_eq_c : cs_lt_hs_c));
end

// Pipeline stage 1
always @(posedge PCLK_OUT_i) begin
    HSYNC_pp[1] <= (h_cnt < H_SYNCLEN) ? 1'b0 : 1'b1;
    if (ds_i2p)
        VSYNC_pp[1] <= (v_cnt < V_SYNCLEN) ? 1'b0 : 1'b1;     // 480i -> 240p: both fields at a line start
    else if (dst_fid == FID_ODD)
        VSYNC_pp[1] <= ((v_cnt < V_SYNCLEN) | ((v_cnt == V_TOTAL) & (h_cnt >= (H_TOTAL/2)))) ? 1'b0 : 1'b1;
    else
        VSYNC_pp[1] <= ((v_cnt < V_SYNCLEN-1) | ((v_cnt == V_SYNCLEN-1) & (h_cnt < (H_TOTAL/2)))) ? 1'b0 : 1'b1;
    DE_pp[1] <= (h_cnt >= H_SYNCLEN+H_BACKPORCH) & (h_cnt < H_SYNCLEN+H_BACKPORCH+H_ACTIVE-panasonic_adjust) & (v_cnt >= V_SYNCLEN+V_BACKPORCH) & (v_cnt < V_SYNCLEN+V_BACKPORCH+V_ACTIVE);

    if (h_avidstart) begin
        // Start 1 line before active so that linebuffer can be filled from DRAM in time
        if (v_avidstart) begin
            ypos_pp[1] <= 11'(-1);
            ypos_pp_init <= 1'b1;
            // Bob deinterlace adjusts linebuf start position and y_ctr for even source fields if
            // output is progressive mode. Noninterlace restore as raw output mode is an exception
            // which ignores LM deinterlace mode setting.
            if (~ext_sync_mode & ~MISC_LM_DEINT_MODE & (Y_RPT > 0) & ~V_INTERLACED & (src_fid == FID_EVEN)) begin
                ypos_lb_next <= 11'(Y_START_LB) - 1'b1;
                y_ctr <= ((Y_RPT+1'b1) >> 1);
                y_ctr_sl_pp[1] <= SL_BOB_ALTERN ? ((Y_RPT+1'b1) >> 1) : '0;
            end else begin
                if (Y_SKIP & (dst_fid == FID_EVEN)) begin
                    // Linedrop mode and output interlaced
                    ypos_lb_next <= 11'(Y_START_LB) + 1'b1;
                end else if ((((Y_RPT == 0) & ~V_INTERLACED) | ((Y_RPT > 0) & MISC_LM_DEINT_MODE)) & (src_fid == FID_EVEN)) begin
                    // Adjust even field Y-offset for noninterlace restore
                    ypos_lb_next <= 11'(Y_START_LB) - MISC_NIR_EVEN_OFFSET;
                end else begin
                    ypos_lb_next <= 11'(Y_START_LB);
                end
                y_ctr <= 0;
                y_ctr_sl_pp[1] <= 0;
            end
            line_id <= ~line_id;
            y_ctr_shmask <= '0;
        end else begin
            if (ypos_pp[1] != V_ACTIVE) begin
                ypos_pp[1] <= ypos_pp[1] + 1'b1;

                if (ypos_pp_init | (y_ctr == Y_RPT) | Y_SKIP) begin
                    if ((ypos_lb_next >= LB_NUM-Y_STEP) & (ypos_lb_next < LB_NUM))
                        ypos_lb_next <= ypos_lb_next + Y_STEP - LB_NUM;
                    else
                        ypos_lb_next <= ypos_lb_next + Y_STEP;
                    ypos_lb <= ypos_lb_next;
                    lb_rd_base <= lb_rd_base_next;
                    line_id <= ~line_id;
                    ypos_pp_init <= 1'b0;
                    if (!ypos_pp_init)
                        y_ctr <= 0;
                end else begin
                    y_ctr <= y_ctr + 1'b1;
                end
                if (!ypos_pp_init) begin
                    y_ctr_sl_pp[1] <= (y_ctr_sl_pp[1] == SL_IV_Y) ? '0 : y_ctr_sl_pp[1] + 1'b1;
                    y_ctr_shmask <= (y_ctr_shmask == MISC_SHMASK_IV_Y) ? '0 : y_ctr_shmask + 1'b1;
                end
            end
        end
        xpos_pp[1] <= 0;
        xpos_lb <= X_START_LB;
        x_ctr <= 0;
        x_ctr_sl_pp[1] <= 0;
        x_ctr_shmask <= 0;
    end else begin
        if (xpos_pp[1] != H_ACTIVE) begin
            xpos_pp[1] <= xpos_pp[1] + 1'b1;
        end

        if (xpos_pp[1] >= xpos_lb_start) begin
            if (x_ctr == X_RPT) begin
                xpos_lb <= xpos_lb + 1'b1;
                x_ctr <= 0;
            end else begin
                x_ctr <= x_ctr + 1'b1;
            end
            x_ctr_sl_pp[1] <= (x_ctr_sl_pp[1] == SL_IV_X) ? '0 : x_ctr_sl_pp[1] + 1'b1;
            x_ctr_shmask <= (x_ctr_shmask == MISC_SHMASK_IV_X) ? '0 : x_ctr_shmask + 1'b1;
        end
    end
end

// Position regeneration (see evh_pp): same update rules as xpos_pp[1] /
// ypos_pp[1], applied PP_TP_START-1 clocks later
integer ev_idx;
always @(posedge PCLK_OUT_i) begin
    evh_pp[1] <= h_avidstart;
    evv_pp[1] <= h_avidstart & v_avidstart;
    for (ev_idx = PP_PL_START+1; ev_idx <= PP_TP_START; ev_idx = ev_idx+1) begin
        evh_pp[ev_idx] <= evh_pp[ev_idx-1];
        evv_pp[ev_idx] <= evv_pp[ev_idx-1];
    end
    if (evh_pp[PP_TP_START-1]) begin
        xpos_tp <= 0;
        if (evv_pp[PP_TP_START-1])
            ypos_tp <= 11'(-1);
        else if (ypos_tp != V_ACTIVE)
            ypos_tp <= ypos_tp + 1'b1;
    end else if (xpos_tp != H_ACTIVE) begin
        xpos_tp <= xpos_tp + 1'b1;
    end
    // PP_PL_END = PP_TP_START + 1
    xpos_end <= xpos_tp;
    ypos_end <= ypos_tp;
end

// Pipeline stages 2-
integer pp_idx;
always @(posedge PCLK_OUT_i) begin
    for(pp_idx = PP_PL_START+1; pp_idx <= PP_PL_END; pp_idx = pp_idx+1) begin
        HSYNC_pp[pp_idx] <= HSYNC_pp[pp_idx-1];
        VSYNC_pp[pp_idx] <= VSYNC_pp[pp_idx-1];
        DE_pp[pp_idx] <= DE_pp[pp_idx-1];
    end
    CSYNC_pp[5] <= cs_d;
    for(pp_idx = 6; pp_idx <= PP_PL_END; pp_idx = pp_idx+1)
        CSYNC_pp[pp_idx] <= CSYNC_pp[pp_idx-1];
    for(pp_idx = PP_PL_START+1; pp_idx <= PP_SLGEN_START; pp_idx = pp_idx+1) begin
        x_ctr_sl_pp[pp_idx] <= x_ctr_sl_pp[pp_idx-1];
        y_ctr_sl_pp[pp_idx] <= y_ctr_sl_pp[pp_idx-1];
    end
    // Overridden later where necessary
    for (pp_idx = PP_SRCSEL_END+1; pp_idx <= PP_PL_END; pp_idx = pp_idx+1) begin
        R_pp[pp_idx] <= R_pp[pp_idx-1];
        G_pp[pp_idx] <= G_pp[pp_idx-1];
        B_pp[pp_idx] <= B_pp[pp_idx-1];
    end

    /* ---------- Mask / Latency tester overlay enable calculation (1 cycle) ---------- */
    if (($signed({1'b0, xpos_pp[PP_MASK_START]}) >= X_OFFSET) &
        ($signed({1'b0, xpos_pp[PP_MASK_START]}) < X_OFFSET+X_SIZE) &
        ($signed({1'b0, ypos_pp[PP_MASK_START]}) >= Y_OFFSET) &
        ($signed({1'b0, ypos_pp[PP_MASK_START]}) < Y_OFFSET+Y_SIZE))
    begin
        mask_enable_pp[PP_MASK_END] <= 1'b0;
    end else begin
        mask_enable_pp[PP_MASK_END] <= 1'b1;
    end
    if ((xpos_pp[PP_MASK_START] >= LT_BOX_X_START) & (xpos_pp[PP_MASK_START] < LT_BOX_X_STOP) &
        (ypos_pp[PP_MASK_START] >= LT_BOX_Y_START) & (ypos_pp[PP_MASK_START] < LT_BOX_Y_STOP))
    begin
        lt_box_enable_pp[PP_MASK_END] <= (lt_mode != 0);
    end else begin
        lt_box_enable_pp[PP_MASK_END] <= 1'b0;
    end
    for(pp_idx = PP_MASK_END+1; pp_idx <= PP_TP_START; pp_idx = pp_idx+1) begin
        mask_enable_pp[pp_idx] <= mask_enable_pp[pp_idx-1];
        lt_box_enable_pp[pp_idx] <= lt_box_enable_pp[pp_idx-1];
    end

    /* ---------- Source selection (1 cycle) ---------- */
    R_pp[PP_SRCSEL_END] <= ext_sync_mode ? ext_R_i : R_linebuf;
    G_pp[PP_SRCSEL_END] <= ext_sync_mode ? ext_G_i : G_linebuf;
    B_pp[PP_SRCSEL_END] <= ext_sync_mode ? ext_B_i : B_linebuf;

    /* ---------- Calculate Y from RGB for hybrid scanlines (2 cycles) ---------- */
    Y_rb_tmp <=  {1'b0, R_pp[PP_Y_CALC_START]} + {1'b0, B_pp[PP_Y_CALC_START]};
    Y <= {1'b0, Y_rb_tmp} + {1'b0, G_pp[PP_Y_CALC_START+1], 1'b0};

    /* ---------- Shadow mask calculation (3 cycles) ---------- */
    R_shmask_str <= shmask_data[10] ? 
                     5'h10 + shmask_data[7:4] :
                     shmask_data[3:0];
    G_shmask_str <= shmask_data[9] ? 
                     5'h10 + shmask_data[7:4] :
                     shmask_data[3:0];
    B_shmask_str <= shmask_data[8] ? 
                     5'h10 + shmask_data[7:4] :
                     shmask_data[3:0];

    // Cycle 3
    R_pp[PP_SHMASK_END] <= MISC_SHMASK_ENABLE ? (R_shmask_mult[8] ? 8'hff : R_shmask_mult[7:0]) : R_pp[PP_SHMASK_START+2];
    G_pp[PP_SHMASK_END] <= MISC_SHMASK_ENABLE ? (G_shmask_mult[8] ? 8'hff : G_shmask_mult[7:0]) : G_pp[PP_SHMASK_START+2];
    B_pp[PP_SHMASK_END] <= MISC_SHMASK_ENABLE ? (B_shmask_mult[8] ? 8'hff : B_shmask_mult[7:0]) : B_pp[PP_SHMASK_START+2];

    /* ---------- Scanline generation (5 cycles) ---------- */
    if (MISC_BFI_ENABLE & bfi_frame) begin
        sl_str <= MISC_BFI_STR;
        sl_method <= 1'b1;
        draw_sl_pp[PP_SLGEN_START+1] <= 1'b1;
    end else if (|(SL_L_OVERLAY & (6'h1<<y_ctr_sl_pp[PP_SLGEN_START]))) begin
        sl_str <= SL_L_STR[y_ctr_sl_pp[PP_SLGEN_START]];
        sl_method <= ~SL_METHOD_PRE;
        draw_sl_pp[PP_SLGEN_START+1] <= 1'b1;
    end else if (|(SL_C_OVERLAY & (10'h1<<x_ctr_sl_pp[PP_SLGEN_START]))) begin
        sl_str <= SL_C_STR[x_ctr_sl_pp[PP_SLGEN_START]];
        sl_method <= ~SL_METHOD_PRE;
        draw_sl_pp[PP_SLGEN_START+1] <= 1'b1;
    end else begin
        draw_sl_pp[PP_SLGEN_START+1] <= 1'b0;
    end
    for (pp_idx = PP_SLGEN_START+2; pp_idx <= PP_SLGEN_END-1; pp_idx = pp_idx+1) begin
        draw_sl_pp[pp_idx] <= draw_sl_pp[pp_idx-1];
    end

    // Cycle 2
    sl_str_thold <= ((sl_str+8'h01)<<4)-1'b1;

    // Cycle 3
    Y_sl_str <= ({1'b0, sl_str_thold} < Y_sl_hybr_ref) ? 8'h0 : sl_str_thold - Y_sl_hybr_ref[7:0];
    R_sl_str <= ({1'b0, sl_str_thold} < R_sl_hybr_ref) ? 8'h0 : sl_str_thold - R_sl_hybr_ref[7:0];
    G_sl_str <= ({1'b0, sl_str_thold} < G_sl_hybr_ref) ? 8'h0 : sl_str_thold - G_sl_hybr_ref[7:0];
    B_sl_str <= ({1'b0, sl_str_thold} < B_sl_hybr_ref) ? 8'h0 : sl_str_thold - B_sl_hybr_ref[7:0];

    // Cycle 4
    // store subtraction based scanlined RGB into pipeline registers
    R_pp[PP_SLGEN_START+4] <= draw_sl_pp[PP_SLGEN_START+3] ? ((R_pp[PP_SLGEN_START+3] > R_sl_str) ? (R_pp[PP_SLGEN_START+3] - R_sl_str) : 8'h00) : R_pp[PP_SLGEN_START+3];
    G_pp[PP_SLGEN_START+4] <= draw_sl_pp[PP_SLGEN_START+3] ? ((G_pp[PP_SLGEN_START+3] > G_sl_str) ? (G_pp[PP_SLGEN_START+3] - G_sl_str) : 8'h00) : G_pp[PP_SLGEN_START+3];
    B_pp[PP_SLGEN_START+4] <= draw_sl_pp[PP_SLGEN_START+3] ? ((B_pp[PP_SLGEN_START+3] > B_sl_str) ? (B_pp[PP_SLGEN_START+3] - B_sl_str) : 8'h00) : B_pp[PP_SLGEN_START+3];

    // Cycle 5
    R_pp[PP_SLGEN_END] <= (draw_sl_pp[PP_SLGEN_START+4] & sl_method) ? R_sl_mult : R_pp[PP_SLGEN_START+4];
    G_pp[PP_SLGEN_END] <= (draw_sl_pp[PP_SLGEN_START+4] & sl_method) ? G_sl_mult : G_pp[PP_SLGEN_START+4];
    B_pp[PP_SLGEN_END] <= (draw_sl_pp[PP_SLGEN_START+4] & sl_method) ? B_sl_mult : B_pp[PP_SLGEN_START+4];

    /* ---------- Testpattern / mask / lt_box generation ---------- */
    R_pp[PP_TP_END] <= lt_active ? {8{lt_box_enable_pp[PP_TP_START]}} : (testpattern_enable ? R_vg : (mask_enable_pp[PP_TP_START] ? MASK_R : R_pp[PP_TP_START]));
    G_pp[PP_TP_END] <= lt_active ? {8{lt_box_enable_pp[PP_TP_START]}} : (testpattern_enable ? G_vg : (mask_enable_pp[PP_TP_START] ? MASK_G : G_pp[PP_TP_START]));
    B_pp[PP_TP_END] <= lt_active ? {8{lt_box_enable_pp[PP_TP_START]}} : (testpattern_enable ? B_vg : (mask_enable_pp[PP_TP_START] ? MASK_B : B_pp[PP_TP_START]));
end

// Output
assign R_o = R_pp[PP_PL_END];
assign G_o = G_pp[PP_PL_END];
assign B_o = B_pp[PP_PL_END];
assign HSYNC_o = HSYNC_pp[PP_PL_END];
assign CSYNC_o = CSYNC_pp[PP_PL_END];
assign VSYNC_o = VSYNC_pp[PP_PL_END];
assign DE_o = DE_pp[PP_PL_END];
assign xpos_o = xpos_end[12] ? 12'hfff : xpos_end[11:0];   // saturate for OSD
assign ypos_o = ypos_end;

endmodule
