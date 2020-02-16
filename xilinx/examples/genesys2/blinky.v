module top(input clkin, clkinb, input [7:0] sw, output [7:0] led);

	wire clk;
	IBUFDS ibuf_i(.I(clkin), .IB(clkinb), .O(clk));

	reg [28:0] ctr;

	always @(posedge clk)
		ctr <= ctr + 1'b1;

	assign led = sw ^ ctr[28:21];
endmodule
