module top(input [3:0] btn, output [3:0] led);
	assign led = btn ^ 4'b1010;
	(* keep *) PS7 ps7_i();
endmodule
