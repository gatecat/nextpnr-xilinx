module INBUF(
    input PAD,
    input VREF,
    input OSC_EN,
    input [3:0] OSC,
    output O
);
    assign O = PAD;
endmodule

module IBUFCTRL(
    input I,
    input IBUFDISABLE,
    input T,
    output O
);
    assign O = (IBUFDISABLE === 1'b1) ? 1'b0 : I;
endmodule

