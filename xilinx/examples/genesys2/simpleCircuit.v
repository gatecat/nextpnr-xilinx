module top (input [7:0] sw, output [4:0] led);
   
assign led = {1'b0,sw[3:0]} + {1'b0,sw[7:4]};

endmodule
