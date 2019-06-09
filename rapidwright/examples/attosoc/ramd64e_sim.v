// Icarus-safe sim models, where the Xilinx ones don't work

module RAMD64E #(
	parameter [63:0] INIT = 64'h0000000000000000,
	parameter [0:0] IS_CLK_INVERTED = 1'b0,
	parameter [1:0] RAM_ADDRESS_MASK = 2'b11,
	parameter [1:0] RAM_ADDRESS_SPACE = 2'b00
) (
	input CLK,
	input I,
	input RADR0, RADR1, RADR2, RADR3, RADR4, RADR5,
	input WADR0, WADR1, WADR2, WADR3, WADR4, WADR5, WADR6, WADR7,
	input WE,
	output O
);
	reg [63:0] ram = INIT;
	wire clk_int = CLK ^ IS_CLK_INVERTED;

	always @(posedge clk_int)
		if (WE && (RAM_ADDRESS_MASK[0] || (WADR6 == RAM_ADDRESS_SPACE[0]))
			   && (RAM_ADDRESS_MASK[1] || (WADR7 == RAM_ADDRESS_SPACE[1])))
			ram[{WADR5, WADR4, WADR3, WADR2, WADR1, WADR0}] <= I;

	assign O = ram[{RADR5, RADR4, RADR3, RADR2, RADR1, RADR0}];

endmodule