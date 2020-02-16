module top(input [7:0] sw, output [7:0] led);
	assign led = sw ^ 8'b10101010;
endmodule
