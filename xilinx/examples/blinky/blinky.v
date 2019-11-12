module top(input clk_i, output [3:0] led);

    BUFGCTRL #(
    .IS_CE1_INVERTED(1'b1),
    .IS_IGNORE0_INVERTED(1'b1),
    .IS_IGNORE1_INVERTED(1'b1),
    .IS_S1_INVERTED(1'b1)
    ) bufg_i (
        .I0(clk_i),
        .CE0(1'b1),
        .CE1(1'b1),
        .IGNORE0(1'b1),
        .IGNORE1(1'b1),
        .S0(1'b1),
        .S1(1'b1),
        .O(clk)
    );

    reg [7:0] ctr;

    always @(posedge clk) ctr <= ctr + 1'b1;

    assign led = ctr[7:4];

endmodule