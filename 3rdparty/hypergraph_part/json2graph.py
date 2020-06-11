#!/usr/bin/env python3

# Convert Yosys JSON to simple text hypergraph for performance testing

import sys, json

node_count = 0
edge2node = {}

netlist = None

with open(sys.argv[1]) as jf:
	netlist = json.load(jf)

top_module = None
for name, module in sorted(netlist["modules"].items()):
	if "attributes" not in module:
		continue
	if "top" not in module["attributes"]:
		continue
	if int(module["attributes"]["top"]) == 0:
		continue
	top_module = module
	break

for cname, cell in sorted(top_module["cells"].items()):
	if "connections" not in cell:
		continue
	for pname, bits in sorted(cell["connections"].items()):
		for bit in bits:
			if bit in ("0", "1", "x", "z"):
				continue
			if bit not in edge2node:
				edge2node[bit] = set()
			edge2node[bit].add(node_count)
	node_count += 1

with open(sys.argv[2], "w") as hf:
	print("{} {}".format(node_count, len(edge2node)), file=hf)
	for n in range(node_count):
		print("N 0 0", file=hf)
	for e, nodes in sorted(edge2node.items()):
		print("E 1 {}".format(" ".join([str(x) for x in sorted(nodes)])), file=hf)
