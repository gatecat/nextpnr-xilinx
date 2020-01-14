module top (input clk_p, clk_n, reset, output [3:0] led);
    wire clk_i;
    IBUFDS clk_ibuf (
        .I(clk_p),
        .IB(clk_n),
        .O(clk_i)
    );
    BUFGCTRL bufg_i (
        .I0(clk_i),
        .CE0(1'b1),
        .S0(1'b1),
        .O(clk)
    );

//`define BLINKY
`ifdef BLINKY
    reg clkdiv;
    reg [21:0] ctr;

    always @(posedge clk) {clkdiv, ctr} <= ctr + 1'b1;

    reg [3:0] led_r;

    always @(posedge clk) begin
        if (clkdiv)
            led_r <= led_r - 1'b1;
    end
    assign led = led_r;
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
