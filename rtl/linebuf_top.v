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
    // ADC samples (h_skip=1), 2 = average of 3 ADC samples (h_skip=2)
    input [1:0] ds_hfilt,
    // Line blending: store the average of each group of (ds_vdec+1) lines
    // (ds_vstart is then the LAST line of the first group)
    input ds_vblend,
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
reg [10:0] xpos_wraddr;
reg [23:0] linebuf_wrdata;
reg linebuf_wren;

// 8192-pixel RAM shared by all layouts. Deeper layouts absorb the read/write
// drift of a non-integer output line period (e.g. 750 input lines -> 262).
reg [12:0] linebuf_wraddr, linebuf_rdaddr;
reg wr_x_ok;
always @(*) begin
    case (lb_layout)
        2'h1: begin
            linebuf_wraddr = {ypos_wraddr[2:0], xpos_wraddr[9:0]};
            linebuf_rdaddr = {ypos_lb[2:0], xpos_lb[9:0]};
            wr_x_ok = (xpos_i[10] == 1'b0);
        end
        2'h2: begin
            linebuf_wraddr = {ypos_wraddr[3:0], xpos_wraddr[8:0]};
            linebuf_rdaddr = {ypos_lb[3:0], xpos_lb[8:0]};
            wr_x_ok = (xpos_i[10:9] == 2'b00);
        end
        default: begin
            linebuf_wraddr = {1'b0, ypos_wraddr[0], xpos_wraddr[10:0]};
            linebuf_rdaddr = {1'b0, ypos_lb[0], xpos_lb[10:0]};
            wr_x_ok = 1'b1;
        end
    endcase
end


linebuf linebuf_rgb (
    .data(linebuf_wrdata),
    .rdaddress(linebuf_rdaddr),
    .rdclock(PCLK_OUT_i),
    .rdclocken(lb_enable),
    .wraddress(linebuf_wraddr),
    .wrclock(PCLK_CAP_i),
    .wren(linebuf_wren),
    .wrclocken(lb_enable),
    .q({R_linebuf, G_linebuf, B_linebuf})
);

// ---------------------------------------------------------------------------
// Downscaler write path
//   stage A: ADC sample history
//   stage B: horizontal sums
//   stage 1: horizontal average, blend RAM read data
//   stage 2: blend (accumulate), blend RAM write
//   stage 3: group average, line buffer write
// ---------------------------------------------------------------------------
wire ds_mode = (ds_vdec != 2'h0);
wire blend_en = ds_vblend & ds_mode;

reg [1:0] ds_gpos;          // line position inside the current group
reg ds_store, ds_first;     // per line: write to line buffer / first line of group

// group start line: ds_vstart for picking, ds_vstart-ds_vdec for blending
wire [10:0] grp_start = {5'h0, ds_vstart} - (blend_en ? {9'h0, ds_vdec} : 11'h0);
wire [1:0] store_gpos = blend_en ? ds_vdec : 2'h0;

// stages A/B: horizontal filter
reg [7:0] R_a, G_a, B_a, R_b, G_b, B_b, R_c, G_c, B_c;
reg [10:0] x_a, x_b;
reg v_a, v_b;
reg [7:0] Rr_b, Gr_b, Br_b;
reg [8:0] s2R_b, s2G_b, s2B_b;
reg [9:0] s3R_b, s3G_b, s3B_b;
wire [17:0] h3R = s3R_b * 8'd171;   // /3 ~ *171/512
wire [17:0] h3G = s3G_b * 8'd171;
wire [17:0] h3B = s3B_b * 8'd171;
wire [7:0] R_h = (ds_hfilt == 2'h1) ? s2R_b[8:1] : ((ds_hfilt == 2'h2) ? h3R[16:9] : Rr_b);
wire [7:0] G_h = (ds_hfilt == 2'h1) ? s2G_b[8:1] : ((ds_hfilt == 2'h2) ? h3G[16:9] : Gr_b);
wire [7:0] B_h = (ds_hfilt == 2'h1) ? s2B_b[8:1] : ((ds_hfilt == 2'h2) ? h3B[16:9] : Br_b);

// blend accumulator: one line, 9 bits per channel
reg [26:0] acc_mem [0:1023] /* synthesis ramstyle = "M9K" */;
reg [26:0] acc_q;

// stage 1 registers
reg [7:0] R_1, G_1, B_1;
reg [10:0] x_1;
reg v_1, store_1, first_1;

always @(posedge PCLK_CAP_i) begin
    // stage A
    {R_a, G_a, B_a} <= {R_i, G_i, B_i};
    {R_b, G_b, B_b} <= {R_a, G_a, B_a};
    {R_c, G_c, B_c} <= {R_b, G_b, B_b};
    x_a <= xpos_i;
    v_a <= DE_i & datavalid_i & wr_x_ok;

    // stage B (R_a = selected sample, R_b/R_c = the previous ADC samples)
    s2R_b <= {1'b0, R_a} + {1'b0, R_b};
    s2G_b <= {1'b0, G_a} + {1'b0, G_b};
    s2B_b <= {1'b0, B_a} + {1'b0, B_b};
    s3R_b <= {2'b0, R_a} + {2'b0, R_b} + {2'b0, R_c};
    s3G_b <= {2'b0, G_a} + {2'b0, G_b} + {2'b0, G_c};
    s3B_b <= {2'b0, B_a} + {2'b0, B_b} + {2'b0, B_c};
    {Rr_b, Gr_b, Br_b} <= {R_a, G_a, B_a};
    x_b <= x_a;
    v_b <= v_a;

    // per-line control
    if (ds_vdec == 2'h0) begin
        // stock: every input line goes to the next buffer
        ds_store <= 1'b1;
        ds_first <= 1'b1;
        if (ypos_i == 0) begin
            ypos_wraddr <= 0;
        end else if (ypos_i != ypos_prev) begin
            if (ypos_wraddr == NUM_LINE_BUFFERS-1)
                ypos_wraddr <= 0;
            else
                ypos_wraddr <= ypos_wraddr + 1'b1;
        end
    end else if (ypos_i != ypos_prev) begin
        // downscaler: lines are grouped by (ds_vdec+1) from grp_start; one
        // line per group is written (picked, or the blended last line).
        // The first stored line (ds_vstart) goes to buffer 0.
        if (ypos_i == grp_start) begin
            ds_gpos <= 2'h0;
        end else if (ds_gpos == ds_vdec) begin
            ds_gpos <= 2'h0;
        end else begin
            ds_gpos <= ds_gpos + 1'b1;
        end

        if (ypos_i == {5'h0, ds_vstart}) begin
            ypos_wraddr <= 0;
            ds_store <= 1'b1;
        end else if ((ypos_i > {5'h0, ds_vstart}) &&
                     (((ypos_i == grp_start) ? 2'h0 : ((ds_gpos == ds_vdec) ? 2'h0 : ds_gpos + 1'b1)) == store_gpos)) begin
            ds_store <= 1'b1;
            if (ypos_wraddr == NUM_LINE_BUFFERS-1)
                ypos_wraddr <= 0;
            else
                ypos_wraddr <= ypos_wraddr + 1'b1;
        end else begin
            ds_store <= 1'b0;
        end

        ds_first <= (ypos_i == grp_start) | (ds_gpos == ds_vdec);
    end
    ypos_prev <= ypos_i;

    // stage B -> 1
    acc_q <= acc_mem[x_b[9:0]];
    R_1 <= R_h;
    G_1 <= G_h;
    B_1 <= B_h;
    x_1 <= x_b;
    v_1 <= v_b;
    store_1 <= ds_store;
    first_1 <= ds_first;
end

// stage 1: accumulate (adders only)
wire [9:0] sR = {1'b0, acc_q[26:18]} + {2'b0, R_1};
wire [9:0] sG = {1'b0, acc_q[17:9]}  + {2'b0, G_1};
wire [9:0] sB = {1'b0, acc_q[8:0]}   + {2'b0, B_1};

// stage 2 registers
reg [9:0] sR_2, sG_2, sB_2;
reg [7:0] R_2, G_2, B_2;
reg [10:0] x_2;
reg v_2, store_2, first_2;

always @(posedge PCLK_CAP_i) begin
    // accumulate the lines of the group that are not written
    if (v_1 & blend_en & ~store_1)
        acc_mem[x_1[9:0]] <= first_1 ? {1'b0, R_1, 1'b0, G_1, 1'b0, B_1} : {sR[8:0], sG[8:0], sB[8:0]};

    // stage 1 -> 2
    {sR_2, sG_2, sB_2} <= {sR, sG, sB};
    {R_2, G_2, B_2} <= {R_1, G_1, B_1};
    x_2 <= x_1;
    v_2 <= v_1;
    store_2 <= store_1;
    first_2 <= first_1;
end

// stage 2: average of the group (2 lines: >>1, 3 lines: *171>>9 ~ /3)
wire [17:0] dR = sR_2 * 8'd171;
wire [17:0] dG = sG_2 * 8'd171;
wire [17:0] dB = sB_2 * 8'd171;
wire [7:0] bR = (ds_vdec == 2'h1) ? sR_2[8:1] : dR[16:9];
wire [7:0] bG = (ds_vdec == 2'h1) ? sG_2[8:1] : dG[16:9];
wire [7:0] bB = (ds_vdec == 2'h1) ? sB_2[8:1] : dB[16:9];

always @(posedge PCLK_CAP_i) begin
    // stage 2 -> 3: line buffer write
    xpos_wraddr <= x_2;
    linebuf_wrdata <= (blend_en & ~first_2) ? {bR, bG, bB} : {R_2, G_2, B_2};
    linebuf_wren <= v_2 & store_2;
end


end
endgenerate

endmodule
