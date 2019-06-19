(* blackbox, keep *)
module IBUFCTRL(
	output O
);
endmodule

module top;
	wire clk_i, clk;
	(* BEL="IOB_X0Y111/IBUFCTRL", keep *)
	IBUFCTRL clk_ibuf (.O(clk_i));

	wire [7:0] led;
	(* BEL="IOB_X0Y110/OUTBUF", keep *)
	OBUF led0_obuf (.I(led[0]));
	(* BEL="IOB_X0Y109/OUTBUF", keep *)
	OBUF led1_obuf (.I(led[1]));
	(* BEL="IOB_X0Y108/OUTBUF", keep *)
	OBUF led2_obuf (.I(led[2]));
	(* BEL="IOB_X0Y107/OUTBUF", keep *)
	OBUF led3_obuf (.I(led[3]));
	(* BEL="IOB_X0Y106/OUTBUF", keep *)
	OBUF led4_obuf (.I(led[4]));
	(* BEL="IOB_X0Y105/OUTBUF", keep *)
	OBUF led5_obuf (.I(led[5]));
	(* BEL="IOB_X0Y104/OUTBUF", keep *)
	OBUF led6_obuf (.I(led[6]));
	(* BEL="IOB_X0Y112/OUTBUF", keep *)
	OBUF led7_obuf (.I(led[7]));

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

	attosoc soc_i(
		.clk(clk),
		.reset(1'b0),
		.led(led)
	);

endmodule