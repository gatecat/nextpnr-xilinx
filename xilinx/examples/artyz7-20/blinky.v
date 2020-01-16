module top(input clk, input [3:0] btn, output [3:0] led);
	reg [27:0] ctr;
	always @(posedge clk)
		ctr <= ctr + 1'b1;

	assign led = ctr[27:24] ^ btn;
	(* keep *) PS7 ps7_i();
endmodule
