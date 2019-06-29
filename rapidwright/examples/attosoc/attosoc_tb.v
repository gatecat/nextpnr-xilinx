module blinky_tb;

	reg clk;

	wire [7:0] led;

	top dut_i(.clk_i(clk), .led(led));

	always #5 clk = (clk === 1'b0);

	always @(led) begin #1 $display("%b", led); end

	initial begin
		$dumpfile("blinky.vcd");
		$dumpvars;
		repeat (50000) @(posedge clk);
		$finish;
	end

endmodule
