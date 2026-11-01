//
// Copyright (C) 2022  Markus Hiienkari <mhiienka@niksula.hut.fi>
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

//`define DEBUG

(* multstyle = "dsp" *)
module linebuf_top (
    input PCLK_CAP_i,
    input PCLK_OUT_i,
    input [7:0] R_i,
    input [7:0] G_i,
    input [7:0] B_i,
    input DE_i,
    input datavalid_i,
    input [11:0] h_in_active,
    input [10:0] xpos_i,
    input [10:0] ypos_i,
    input [10:0] xpos_lb,
    input [10:0] ypos_lb,
    input [10:0] ypos_lb_next,
    input line_id,
    input lb_enable,
    // Downscaler: store only every (ds_vdec+1)th input line, starting at
    // input line ds_vstart. ds_vdec=0 keeps the stock behaviour.
    input [1:0] ds_vdec,
    input [5:0] ds_vstart,
    // Line buffer layout: 0 = 2 x 2048 px (stock), 1 = 8 x 1024 px,
    // 2 = 16 x 512 px (deep, for large read/write drift)
    input [1:0] lb_layout,
    // Horizontal box filter for oversampled input: 0 = off, 1 = average of 2
    // ADC samples (h_skip=1), 2 = average of 4 (h_skip=3)
    input [1:0] ds_hfilt,
    // Vertical line selection. Input lines are grouped by (ds_vdec+1) starting
    // at line ds_vstart; in each group the line at position st (0..ds_vdec) is
    // written, as the average of the n lines ending at st (n = 1: plain pick,
    // n = 0 means 4).
    // ds_field_cfg = {fieldB_n[1:0], fieldB_st[1:0], fieldA_n[1:0], fieldA_st[1:0]}
    // Field B settings apply to every other input frame in interlaced output.
    input [7:0] ds_field_cfg,
    input ds_ilace,
    input in_field,             // input frame parity (output clock domain)
    // layout 3 (YCbCr 4:2:2, or RGB one pixel per word with lb_rgb): words
    // per line, number of lines, read base
    input [9:0] lb_stride,
    input [5:0] lb_slots,
    input lb_rgb,
    input [12:0] lb_rd_base,
    output [7:0] R_linebuf,
    output [7:0] G_linebuf,
    output [7:0] B_linebuf,
    // optional EMIF if
    input emif_br_clk,
    input emif_br_reset,
    output reg [27:0] emif_rd_addr,
    output reg emif_rd_read,
    input [255:0] emif_rd_rdata,
    input emif_rd_waitrequest,
    input emif_rd_readdatavalid,
    output reg [5:0] emif_rd_burstcount,
    output [27:0] emif_wr_addr,
    output reg emif_wr_write,
    output [255:0] emif_wr_wdata,
    input emif_wr_waitrequest,
    output reg [5:0] emif_wr_burstcount
);

parameter EMIF_ENABLE = 0;
parameter NUM_LINE_BUFFERS = 32;


localparam EMIF_WR_MAXBURST = 32;
localparam EMIF_RD_MAXBURST = 32;

generate
if (EMIF_ENABLE) begin

`ifdef DEBUG
reg [10:0] emif_wr_stall_ctr /* synthesis noprune */;
reg [10:0] emif_rd_stall_ctr /* synthesis noprune */;
reg emif_rd_line_missed /* synthesis noprune */;

always @(posedge emif_br_clk) begin
    if (emif_wr_write & emif_wr_waitrequest & (emif_wr_stall_ctr != 2047))
        emif_wr_stall_ctr <= emif_wr_stall_ctr + 1'b1;
    else
        emif_wr_stall_ctr <= 0;

    if (emif_rd_burstcount > 0) begin
        if (emif_rd_readdatavalid)
            emif_rd_stall_ctr <= 0;
        else
            emif_rd_stall_ctr <= emif_rd_stall_ctr + 1'b1;
    end
end
`endif

// Number of 8-pixel blocks per line
wire [8:0] blocks_per_line = (h_in_active / 8) + (h_in_active[2:0] != 3'h0);

/* ------------------------------ EMIF WR interface ------------------------------ */

// PCLK_CAP_i related signals
reg [8:0] emif_wr_fifo_blksleft;
reg [19:0] emif_wr_fifo_addr; // [19:9] = y_pos; [8:0] = x_pos_x8
reg [23:0] emif_wr_fifo_pixs[7:0] /* synthesis ramstyle = "logic" */;
reg emif_wr_fifo_wrreq;
reg [8:0] blocks_inserted;
reg DE_prev;
wire emif_wr_fifo_wrfull;

// emif_br_clk related signals
wire [8:0] emif_wr_fifo_blksleft_q;
reg [8:0] emif_wr_blksleft;
wire [19:0] emif_wr_fifo_addr_q;
wire [23:0] emif_wr_fifo_pixs_q[7:0];
reg emif_wr_fifo_iniread_prev;
wire emif_wr_fifo_rdempty;
wire [8:0] emif_wr_fifo_rdusedw;
wire emif_wr_fifo_iniread = (lb_enable & (emif_wr_blksleft == 0) & !emif_wr_fifo_iniread_prev & !emif_wr_fifo_rdempty);
wire emif_wr_fifo_rdreq = emif_wr_fifo_iniread | ((emif_wr_burstcount > 1) & !emif_wr_waitrequest);

assign emif_wr_addr = {3'b001, emif_wr_fifo_addr_q, 5'h0};
assign emif_wr_wdata = {8'h0, emif_wr_fifo_pixs_q[0], 8'h0, emif_wr_fifo_pixs_q[1], 8'h0, emif_wr_fifo_pixs_q[2], 8'h0, emif_wr_fifo_pixs_q[3],
                        8'h0, emif_wr_fifo_pixs_q[4], 8'h0, emif_wr_fifo_pixs_q[5], 8'h0, emif_wr_fifo_pixs_q[6], 8'h0, emif_wr_fifo_pixs_q[7]};

dc_fifo_emif_wr dc_fifo_emif_wr_inst (
    .data({emif_wr_fifo_blksleft, emif_wr_fifo_addr,
           emif_wr_fifo_pixs[0], emif_wr_fifo_pixs[1], emif_wr_fifo_pixs[2], emif_wr_fifo_pixs[3],
           emif_wr_fifo_pixs[4], emif_wr_fifo_pixs[5], emif_wr_fifo_pixs[6], emif_wr_fifo_pixs[7]}),
    .rdclk(emif_br_clk),
    .rdreq(emif_wr_fifo_rdreq),
    .rdempty(emif_wr_fifo_rdempty),
    .rdusedw(emif_wr_fifo_rdusedw),
    .wrclk(PCLK_CAP_i),
    .wrreq(emif_wr_fifo_wrreq),
    .wrfull(emif_wr_fifo_wrfull),
    .q({emif_wr_fifo_blksleft_q, emif_wr_fifo_addr_q,
        emif_wr_fifo_pixs_q[0], emif_wr_fifo_pixs_q[1], emif_wr_fifo_pixs_q[2], emif_wr_fifo_pixs_q[3],
        emif_wr_fifo_pixs_q[4], emif_wr_fifo_pixs_q[5], emif_wr_fifo_pixs_q[6], emif_wr_fifo_pixs_q[7]})
);

always @(posedge PCLK_CAP_i) begin
    emif_wr_fifo_wrreq <= 1'b0;

    if (datavalid_i) begin
        emif_wr_fifo_pixs[xpos_i[2:0]] <= {R_i, G_i, B_i};
        DE_prev <= DE_i;

        if (~DE_prev & DE_i)
            blocks_inserted <= 0;

        if ((blocks_inserted < blocks_per_line) & ~emif_wr_fifo_wrfull & (xpos_i[2:0] == 3'h7)) begin
            emif_wr_fifo_blksleft <= blocks_per_line - blocks_inserted;
            emif_wr_fifo_addr[19:9] <= ypos_i;
            emif_wr_fifo_addr[7:0] <= xpos_i[10:3];
            emif_wr_fifo_wrreq <= 1'b1;
            blocks_inserted <= blocks_inserted + 1'b1;
        end
    end
end

always @(posedge emif_br_clk or posedge emif_br_reset) begin
    if (emif_br_reset) begin
        emif_wr_write <= 1'b0;
        emif_wr_burstcount <= 1'b0;
    end else begin
        if (emif_wr_burstcount > 0) begin
            if (!emif_wr_waitrequest) begin
                emif_wr_burstcount <= emif_wr_burstcount - 1'b1;
                if (emif_wr_burstcount == 1) begin
                    emif_wr_blksleft <= 0;
                    emif_wr_write <= 1'b0;
                end
            end
        end else if (lb_enable & (emif_wr_blksleft > 0) & ((emif_wr_fifo_rdusedw >= emif_wr_blksleft-1) | (emif_wr_fifo_rdusedw >= 31))) begin
            emif_wr_write <= 1'b1;
            emif_wr_burstcount <= (emif_wr_blksleft > EMIF_WR_MAXBURST) ? EMIF_WR_MAXBURST : emif_wr_blksleft;
        end else if (emif_wr_fifo_iniread_prev) begin
            emif_wr_blksleft <= emif_wr_fifo_blksleft_q;
        end

        emif_wr_fifo_iniread_prev <= emif_wr_fifo_iniread;
    end
end

/* ------------------------------ EMIF RD interface ------------------------------ */

// PCLK_OUT_i related signals
wire [12:0] linebuf_rdaddr = {xpos_lb + (line_id ? 2560 : 0)};

// emif_br_clk related signals
reg [8:0] blocks_copied;
wire linebuf_wren = emif_rd_readdatavalid;
wire [191:0] linebuf_wrdata = {emif_rd_rdata[23:0], emif_rd_rdata[55:32], emif_rd_rdata[87:64], emif_rd_rdata[119:96],
                               emif_rd_rdata[151:128], emif_rd_rdata[183:160], emif_rd_rdata[215:192], emif_rd_rdata[247:224]};
wire [9:0] linebuf_wraddr = {blocks_copied + (line_id ? 0 : 320)};
wire [19:0] emif_rd_block_addr = {ypos_lb_next, blocks_copied};
reg line_id_brclk_sync1_reg, line_id_brclk_sync2_reg, line_id_brclk_sync3_reg;


linebuf_double linebuf_rgb (
    .data(linebuf_wrdata),
    .rdaddress(linebuf_rdaddr),
    .rdclock(PCLK_OUT_i),
    .rdclocken(lb_enable),
    .wraddress(linebuf_wraddr),
    .wrclock(emif_br_clk),
    .wren(linebuf_wren),
    .wrclocken(lb_enable),
    .q({R_linebuf, G_linebuf, B_linebuf})
);

// BRAM linebuffer operation
always @(posedge emif_br_clk or posedge emif_br_reset) begin
    if (emif_br_reset) begin
        emif_rd_read <= 1'b0;
        emif_rd_burstcount <= 1'b0;
    end else begin
        if (emif_rd_burstcount > 0) begin // always finish read first, make sure doesn't last longer than line length
            if (emif_rd_readdatavalid) begin
                blocks_copied <= blocks_copied + 1'b1;
                emif_rd_burstcount <= emif_rd_burstcount - 1'b1;
            end
            if (!emif_rd_waitrequest)
                emif_rd_read <= 1'b0;
`ifdef DEBUG
            emif_rd_line_missed <= (line_id_brclk_sync2_reg != line_id_brclk_sync3_reg);
`endif
        end else if (line_id_brclk_sync2_reg != line_id_brclk_sync3_reg) begin
            blocks_copied <= 0;
        end else if (lb_enable & (blocks_copied < blocks_per_line)) begin
            emif_rd_read <= 1'b1;
            emif_rd_burstcount <= ((blocks_per_line-blocks_copied) > EMIF_RD_MAXBURST) ? EMIF_RD_MAXBURST : (blocks_per_line-blocks_copied);
            emif_rd_addr <= {3'b001, emif_rd_block_addr, 5'h0};
        end

        line_id_brclk_sync1_reg <= line_id;
        line_id_brclk_sync2_reg <= line_id_brclk_sync1_reg;
        line_id_brclk_sync3_reg <= line_id_brclk_sync2_reg;
    end
end

end else begin // EMIF_ENABLE

/* ------------------------------ BRAM lb interface ------------------------------ */

reg [5:0] ypos_wraddr;
reg [10:0] ypos_prev;
reg [12:0] linebuf_wraddr_r;
reg [31:0] linebuf_wrdata;
reg linebuf_wren;

// 8192 x 32 RAM shared by all layouts. Layouts 0-2 store one RGB pixel per
// word (2 x 2048, 8 x 1024, 16 x 512 pixels); layout 3 stores two pixels per
// word in YCbCr 4:2:2 {Y0, Y1, Cb, Cr} with a runtime line stride (lb_stride
// words per line, lb_slots lines). Deeper layouts absorb the read/write drift
// of a non-integer output line period (e.g. 750 input lines -> 262).
wire lbs = (lb_layout == 2'h3);     // runtime stride layout
wire lb422 = lbs & ~lb_rgb;
reg [12:0] linebuf_rdaddr;
reg wr_x_ok;
always @(*) begin
    case (lb_layout)
        2'h1: begin
            linebuf_rdaddr = {ypos_lb[2:0], xpos_lb[9:0]};
            wr_x_ok = (xpos_i[10] == 1'b0);
        end
        2'h2: begin
            linebuf_rdaddr = {ypos_lb[3:0], xpos_lb[8:0]};
            wr_x_ok = (xpos_i[10:9] == 2'b00);
        end
        2'h3: begin
            linebuf_rdaddr = lb_rd_base + (lb_rgb ? {2'h0, xpos_lb} : {3'h0, xpos_lb[10:1]});
            wr_x_ok = 1'b1;
        end
        default: begin
            linebuf_rdaddr = {1'b0, ypos_lb[0], xpos_lb[10:0]};
            wr_x_ok = 1'b1;
        end
    endcase
end

wire [31:0] linebuf_q;

linebuf linebuf_rgb (
    .data(linebuf_wrdata),
    .rdaddress(linebuf_rdaddr),
    .rdclock(PCLK_OUT_i),
    .rdclocken(lb_enable),
    .wraddress(linebuf_wraddr_r),
    .wrclock(PCLK_CAP_i),
    .wren(linebuf_wren),
    .wrclocken(lb_enable),
    .q(linebuf_q)
);

// ---------------------------------------------------------------------------
// Read side (output clock): layout 3 is converted back to RGB. Both paths
// have the same latency (2 clocks after the RAM output); see PP_LINEBUF_LENGTH
// in scanconverter.
// ---------------------------------------------------------------------------
reg [1:0] rd_x0_d;
reg rd_x0;
reg [7:0] o_Y;
reg signed [9:0] o_cb, o_cr;
reg signed [17:0] o_rt, o_gt, o_bt;
reg [23:0] o_raw1, o_raw2;
reg [7:0] o_R, o_G, o_B;
reg o_422_1;
wire signed [9:0] cb_s = $signed({2'b00, linebuf_q[15:8]}) - 10'sd128;
wire signed [9:0] cr_s = $signed({2'b00, linebuf_q[7:0]}) - 10'sd128;
wire signed [10:0] o_Rs = $signed({3'b000, o_Y}) + $signed(o_rt[17:8]);
wire signed [10:0] o_Gs = $signed({3'b000, o_Y}) + $signed(o_gt[17:8]);
wire signed [10:0] o_Bs = $signed({3'b000, o_Y}) + $signed(o_bt[17:8]);
function [7:0] clamp8;
    input signed [10:0] v;
    clamp8 = v[10] ? 8'h00 : (|v[9:8] ? 8'hff : v[7:0]);
endfunction

always @(posedge PCLK_OUT_i) begin
    // pixel parity follows the read address through the RAM (2 clocks)
    rd_x0_d <= {rd_x0_d[0], xpos_lb[0]};
    // conversion stage 1: R = Y + 1.402 Cr, G = Y - 0.344 Cb - 0.714 Cr, B = Y + 1.772 Cb
    o_Y <= rd_x0_d[1] ? linebuf_q[23:16] : linebuf_q[31:24];
    o_rt <= cr_s * 18'sd359;
    o_gt <= -(cb_s * 18'sd88) - (cr_s * 18'sd183);
    o_bt <= cb_s * 18'sd454;
    o_raw1 <= linebuf_q[23:0];
    o_422_1 <= lb422;
    // conversion stage 2
    if (o_422_1)
        {o_R, o_G, o_B} <= {clamp8(o_Rs), clamp8(o_Gs), clamp8(o_Bs)};
    else
        {o_R, o_G, o_B} <= o_raw1;
end

assign {R_linebuf, G_linebuf, B_linebuf} = {o_R, o_G, o_B};

// ---------------------------------------------------------------------------
// Downscaler write path
//   stage A: ADC sample history
//   stage B: horizontal sums
//   stage 1: horizontal average -> pixel
//   stage U0: storage unit: RGB pixel, or YCbCr 4:2:2 pixel pair (layout 3,
//             two extra stages C1/C2 before it); blend RAM read
//   stage U1: blend (accumulate), blend RAM write
//   stage U2: group average, line buffer write
// ---------------------------------------------------------------------------
wire ds_mode = (ds_vdec != 2'h0);
reg [1:0] in_field_sync;
always @(posedge PCLK_CAP_i)
    in_field_sync <= {in_field_sync[0], in_field};

wire fld_b = ds_ilace & in_field_sync[1];
wire [1:0] st_gpos = fld_b ? ds_field_cfg[5:4] : ds_field_cfg[1:0];
wire [1:0] n_field = fld_b ? ds_field_cfg[7:6] : ds_field_cfg[3:2];
wire [2:0] n_blend = (n_field == 2'h0) ? 3'h4 : {1'b0, n_field};      // 1..4
wire [1:0] first_gpos = st_gpos - n_blend[1:0] + 2'h1;
wire blend_en = ds_mode & (n_blend > 3'h1);

reg [1:0] ds_gpos;          // line position inside the current group
reg ds_store;               // per line: write this line to the line buffer
reg ds_first, ds_inblend;   // per line: first line of the blend window / inside it
reg ds_halve;               // per line: 4-line blend, 3rd line (keep sum/2 in 9 bits)

wire [10:0] grp_start = {5'h0, ds_vstart};
wire [10:0] first_store = grp_start + {9'h0, st_gpos};
wire [1:0] next_gpos = (ypos_i == grp_start) ? 2'h0 : ((ds_gpos == ds_vdec) ? 2'h0 : ds_gpos + 2'h1);
wire [5:0] lb_num = lbs ? lb_slots : NUM_LINE_BUFFERS;

// stages A/B: horizontal filter
reg [7:0] R_a, G_a, B_a, R_b, G_b, B_b, R_c, G_c, B_c, R_d, G_d, B_d;
reg [10:0] x_a, x_b;
reg v_a, v_b;
reg [7:0] Rr_b, Gr_b, Br_b;
reg [8:0] s2R_b, s2G_b, s2B_b, s2R_cd, s2G_cd, s2B_cd;
wire [9:0] s4R = {1'b0, s2R_b} + {1'b0, s2R_cd};
wire [9:0] s4G = {1'b0, s2G_b} + {1'b0, s2G_cd};
wire [9:0] s4B = {1'b0, s2B_b} + {1'b0, s2B_cd};
wire [7:0] R_h = (ds_hfilt == 2'h1) ? s2R_b[8:1] : ((ds_hfilt == 2'h2) ? s4R[9:2] : Rr_b);
wire [7:0] G_h = (ds_hfilt == 2'h1) ? s2G_b[8:1] : ((ds_hfilt == 2'h2) ? s4G[9:2] : Gr_b);
wire [7:0] B_h = (ds_hfilt == 2'h1) ? s2B_b[8:1] : ((ds_hfilt == 2'h2) ? s4B[9:2] : Br_b);

// stage 1 registers
reg [7:0] R_1, G_1, B_1;
reg [10:0] x_1;
reg v_1;

// line buffer base address of the line being written (layout 3)
reg [12:0] wr_base;

always @(posedge PCLK_CAP_i) begin
    // stage A
    {R_a, G_a, B_a} <= {R_i, G_i, B_i};
    {R_b, G_b, B_b} <= {R_a, G_a, B_a};
    {R_c, G_c, B_c} <= {R_b, G_b, B_b};
    {R_d, G_d, B_d} <= {R_c, G_c, B_c};
    x_a <= xpos_i;
    v_a <= DE_i & datavalid_i & wr_x_ok;

    // stage B (R_a = selected sample, R_b/R_c = the previous ADC samples)
    s2R_b <= {1'b0, R_a} + {1'b0, R_b};
    s2G_b <= {1'b0, G_a} + {1'b0, G_b};
    s2B_b <= {1'b0, B_a} + {1'b0, B_b};
    s2R_cd <= {1'b0, R_c} + {1'b0, R_d};
    s2G_cd <= {1'b0, G_c} + {1'b0, G_d};
    s2B_cd <= {1'b0, B_c} + {1'b0, B_d};
    {Rr_b, Gr_b, Br_b} <= {R_a, G_a, B_a};
    x_b <= x_a;
    v_b <= v_a;

    // per-line control
    if (ds_vdec == 2'h0) begin
        // stock: every input line goes to the next buffer
        ds_store <= 1'b1;
        ds_first <= 1'b1;
        ds_inblend <= 1'b0;
        ds_halve <= 1'b0;
        if (ypos_i == 0) begin
            ypos_wraddr <= 0;
        end else if (ypos_i != ypos_prev) begin
            if (ypos_wraddr == lb_num-1'b1)
                ypos_wraddr <= 0;
            else
                ypos_wraddr <= ypos_wraddr + 1'b1;
        end
    end else if (ypos_i != ypos_prev) begin
        // downscaler: lines are grouped by (ds_vdec+1) from grp_start; one
        // line per group is written. The first written line goes to buffer 0.
        ds_gpos <= next_gpos;

        if (ypos_i == first_store) begin
            ypos_wraddr <= 0;
            ds_store <= 1'b1;
        end else if ((ypos_i > first_store) && (next_gpos == st_gpos)) begin
            ds_store <= 1'b1;
            if (ypos_wraddr == lb_num-1'b1)
                ypos_wraddr <= 0;
            else
                ypos_wraddr <= ypos_wraddr + 1'b1;
        end else begin
            ds_store <= 1'b0;
        end

        ds_first <= (next_gpos == first_gpos);
        ds_halve <= (n_blend == 3'h4) & (next_gpos == first_gpos + 2'h2);
        ds_inblend <= blend_en & (next_gpos >= first_gpos) & (next_gpos <= st_gpos);
    end
    ypos_prev <= ypos_i;
    // (changes during horizontal blanking only, long before the first write)
    wr_base <= ypos_wraddr * lb_stride;

    // stage B -> 1
    R_1 <= R_h;
    G_1 <= G_h;
    B_1 <= B_h;
    x_1 <= x_b;
    v_1 <= v_b;
end

// YCbCr 4:2:2 (layout 3): BT.601 full range, 8.8 fixed point
reg [15:0] cy_Y;
reg [16:0] cy_Cb, cy_Cr;
reg [10:0] x_c1;
reg v_c1;
reg [7:0] p_Y0, p_Cb0, p_Cr0;
wire [7:0] c2_Y = cy_Y[15:8] + cy_Y[7];
wire [7:0] c2_Cb = cy_Cb[15:8];
wire [7:0] c2_Cr = cy_Cr[15:8];
wire [8:0] c2_Cbs = {1'b0, p_Cb0} + {1'b0, c2_Cb};
wire [8:0] c2_Crs = {1'b0, p_Cr0} + {1'b0, c2_Cr};

// storage unit stream: 4 x 8-bit channels
reg [31:0] u0, u1;
reg [10:0] ua0, ua1;
reg uv0, uv1;
reg ublend1;

// blend accumulator: one line of units, 9 bits per channel (RGB: 768
// pixels, 4:2:2: 768 pixel pairs)
(* ramstyle = "M9K", max_depth = 256 *) reg [35:0] acc_mem [0:767];   // 3 x (256 x 36)
reg [35:0] acc_q;

always @(posedge PCLK_CAP_i) begin
    // C1: color space conversion
    cy_Y  <= 16'd77*R_1 + 16'd150*G_1 + 16'd29*B_1;
    cy_Cb <= 17'd32768 + 17'd128*B_1 - 17'd43*R_1 - 17'd85*G_1;
    cy_Cr <= 17'd32768 + 17'd128*R_1 - 17'd107*G_1 - 17'd21*B_1;
    x_c1 <= x_1;
    v_c1 <= v_1;

    // C2 / U0: storage unit
    if (lb422) begin
        if (v_c1 & ~x_c1[0])
            {p_Y0, p_Cb0, p_Cr0} <= {c2_Y, c2_Cb, c2_Cr};
        u0 <= {p_Y0, c2_Y, c2_Cbs[8:1], c2_Crs[8:1]};
        ua0 <= {1'b0, x_c1[10:1]};
        uv0 <= v_c1 & x_c1[0] & (x_c1[10:1] < lb_stride);
    end else begin
        u0 <= {R_1, G_1, B_1, 8'h0};
        ua0 <= x_1;
        uv0 <= v_1 & (~lbs | (x_1 < {1'b0, lb_stride}));
    end

    // U0 -> U1 (blend RAM read)
    acc_q <= acc_mem[ua0[9:0]];
    u1 <= u0;
    ua1 <= ua0;
    uv1 <= uv0;
end

// U1: accumulate (adders only)
wire [9:0] s0 = {1'b0, acc_q[35:27]} + {2'b0, u1[31:24]};
wire [9:0] s1 = {1'b0, acc_q[26:18]} + {2'b0, u1[23:16]};
wire [9:0] s2 = {1'b0, acc_q[17:9]}  + {2'b0, u1[15:8]};
wire [9:0] s3 = {1'b0, acc_q[8:0]}   + {2'b0, u1[7:0]};

// 4-line blend at the written line: 2 x (sum of 3 lines / 2) + this line
wire [10:0] t0 = {1'b0, acc_q[35:27], 1'b0} + {3'b0, u1[31:24]};
wire [10:0] t1 = {1'b0, acc_q[26:18], 1'b0} + {3'b0, u1[23:16]};
wire [10:0] t2 = {1'b0, acc_q[17:9], 1'b0}  + {3'b0, u1[15:8]};
wire [10:0] t3 = {1'b0, acc_q[8:0], 1'b0}   + {3'b0, u1[7:0]};

// U2 registers
reg [9:0] s0_2, s1_2, s2_2, s3_2;
reg [7:0] q0_2, q1_2, q2_2, q3_2;
reg [31:0] u2;
reg [10:0] ua2;
reg uv2, blended_2;

always @(posedge PCLK_CAP_i) begin
    // accumulate the lines of the blend window that are not written
    if (uv1 & ds_inblend & ~ds_store & (ua1 < 11'd768))
        acc_mem[ua1[9:0]] <= ds_first ? {1'b0, u1[31:24], 1'b0, u1[23:16], 1'b0, u1[15:8], 1'b0, u1[7:0]} :
                             (ds_halve ? {s0[9:1], s1[9:1], s2[9:1], s3[9:1]} : {s0[8:0], s1[8:0], s2[8:0], s3[8:0]});

    {s0_2, s1_2, s2_2, s3_2} <= {s0, s1, s2, s3};
    {q0_2, q1_2, q2_2, q3_2} <= {t0[9:2], t1[9:2], t2[9:2], t3[9:2]};
    u2 <= u1;
    ua2 <= ua1;
    uv2 <= uv1 & ds_store;
    blended_2 <= ds_inblend & ~ds_first;
end

// U2: average of the blend window (2 lines: >>1, 3 lines: *171>>9 ~ /3,
// 4 lines: see t0..t3)
wire [17:0] d0 = s0_2 * 8'd171;
wire [17:0] d1 = s1_2 * 8'd171;
wire [17:0] d2 = s2_2 * 8'd171;
wire [17:0] d3 = s3_2 * 8'd171;
wire [7:0] b0 = (n_blend == 3'h2) ? s0_2[8:1] : ((n_blend == 3'h4) ? q0_2 : d0[16:9]);
wire [7:0] b1 = (n_blend == 3'h2) ? s1_2[8:1] : ((n_blend == 3'h4) ? q1_2 : d1[16:9]);
wire [7:0] b2 = (n_blend == 3'h2) ? s2_2[8:1] : ((n_blend == 3'h4) ? q2_2 : d2[16:9]);
wire [7:0] b3 = (n_blend == 3'h2) ? s3_2[8:1] : ((n_blend == 3'h4) ? q3_2 : d3[16:9]);
wire [31:0] u2_out = blended_2 ? {b0, b1, b2, b3} : u2;

always @(posedge PCLK_CAP_i) begin
    // U2 -> line buffer write
    case (lb_layout)
        2'h1: linebuf_wraddr_r <= {ypos_wraddr[2:0], ua2[9:0]};
        2'h2: linebuf_wraddr_r <= {ypos_wraddr[3:0], ua2[8:0]};
        2'h3: linebuf_wraddr_r <= wr_base + {2'h0, ua2};
        default: linebuf_wraddr_r <= {1'b0, ypos_wraddr[0], ua2};
    endcase
    linebuf_wrdata <= lb422 ? u2_out : {8'h0, u2_out[31:8]};
    linebuf_wren <= uv2;
end

end
endgenerate

endmodule
