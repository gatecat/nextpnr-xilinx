module IBUFCTRL(
    input I,
    output O
);
endmodule

module top;
    wire clk_i, clk;
    // PMOD0, pin1
    (* BEL="IOB_X3Y245/IBUFCTRL", keep *)
    IBUFCTRL clk_ibuf (.O(clk_i));

    wire [3:0] led;
    (* BEL="IOB_X3Y295/OUTBUF", keep *)
    OBUF led0_obuf (.I(led[0]));
    (* BEL="IOB_X3Y294/OUTBUF", keep *)
    OBUF led1_obuf (.I(led[1]));
    (* BEL="IOB_X3Y293/OUTBUF", keep *)
    OBUF led2_obuf (.I(led[2]));
    (* BEL="IOB_X3Y292/OUTBUF", keep *)
    OBUF led3_obuf (.I(led[3]));

    BUFGCTRL bufg_i (
        .I0(clk_i),
        .CE0(1'b1),
        .CE1(1'b0),
        .IGNORE0(1'b0),
        .IGNORE1(1'b0),
        .S0(1'b1),
        .S1(1'b0),
        .O(clk)
    );
//`define BLINKY
`ifdef BLINKY
    reg clkdiv;
    reg [19:0] ctr;

    always @(posedge clk) {clkdiv, ctr} <= ctr + 1'b1;

    reg [4:0] led_r;

    always @(posedge clk) begin
        if (clkdiv)
            led_r <= led_r - 1'b1;
    end
    assign led = &led_r;
`else
    wire [7:0] soc_led;

    attosoc soc_i(
        .clk(clk),
        .led(soc_led)
    );

    assign led = soc_led[3:0];
`endif
endmodule