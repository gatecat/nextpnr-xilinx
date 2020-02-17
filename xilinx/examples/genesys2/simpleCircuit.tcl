open_checkpoint simpleCircuit.dcp
write_verilog -force simpleCircuit_out.v
set_property IS_LOC_FIXED true [get_ports]
write_bitstream -force simpleCircuit.bit
