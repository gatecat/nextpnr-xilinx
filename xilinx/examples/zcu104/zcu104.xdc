set_property LOC D5 [get_ports led[0]]
set_property LOC D6 [get_ports led[1]]
set_property LOC A5 [get_ports led[2]]
set_property LOC B5 [get_ports led[3]]

set_property IOSTANDARD LVCMOS33 [get_ports led[0]]
set_property IOSTANDARD LVCMOS33 [get_ports led[1]]
set_property IOSTANDARD LVCMOS33 [get_ports led[2]]
set_property IOSTANDARD LVCMOS33 [get_ports led[3]]

set_property LOC M11 [get_ports reset]
set_property IOSTANDARD LVCMOS33 [get_ports reset]

set_property LOC F23 [get_ports clk_p]
set_property LOC E23 [get_ports clk_n]
set_property IOSTANDARD LVDS [get_ports clk_p]
