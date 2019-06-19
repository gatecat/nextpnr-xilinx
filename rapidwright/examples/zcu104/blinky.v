module top (input reset, output [3:0] led);

    // Use PS8 to get a 100MHz clock
    wire [3:0] plclk;
    PS8 ps_i(
        .PLCLK(plclk)
    );

    (* BEL="BUFG_PS_X0Y91/BUFG_PS" *)
    BUFG_PS bfg_i(
        .I(plclk[0]),
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
        .reset(reset),
        .led(soc_led)
    );

    assign led = soc_led[3:0];
`endif
endmodule