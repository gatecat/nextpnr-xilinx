module blinky_tb;

	reg clk;

	wire [7:0] led;

	top dut_i(.clk_ibuf(clk),
		.led0_obuf(led[0]), .led1_obuf(led[1]), .led2_obuf(led[2]), .led3_obuf(led[3]),
		.led4_obuf(led[4]), .led5_obuf(led[5]), .led6_obuf(led[6]), .led7_obuf(led[7]));

	always #5 clk = (clk === 1'b0);

	always @(led) begin #1 $display("%b", led); end

	initial begin
		$dumpfile("blinky.vcd");
		$dumpvars;
		repeat (50000) @(posedge clk);
		$finish;
	end

endmodule
