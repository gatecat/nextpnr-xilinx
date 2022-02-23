from xilinx_device import *
from bba import BBAWriter
import sys, argparse
import bels, constid
from nextpnr_structs import *
import os

def main():

	rwbase = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")

	parser = argparse.ArgumentParser()
	parser.add_argument("--xray", help="Project X-Ray device database path for current family (e.g. ../prjxray-db/artix7)", type=str, default=os.path.join(rwbase, "external", "prjxray-db", "artix7"))
	parser.add_argument("--metadata", help="nextpnr-xilinx site metadata root", type=str, default=os.path.join(rwbase, "external", "nextpnr-xilinx-meta", "artix7"))
	parser.add_argument("--device", help="name of device to export", type=str, required=True)
	parser.add_argument("--constids", help="name of nextpnr constids file to read", type=str, default=os.path.join(rwbase, "constids.inc"))
	parser.add_argument("--bba", help="bba file to write", type=str, required=True)
	args = parser.parse_args()
	# Read baked-in constids
	with open(args.constids, "r") as cf:
		constid.read_base(cf)
	# Read and parse X-ray database
	metadata_root = args.metadata
	xraydb_root = args.xray
	if "xc7z" in args.device:
		metadata_root = metadata_root.replace("artix7", "zynq7")
		xraydb_root = xraydb_root.replace("artix7", "zynq7")
	if "xc7k" in args.device:
		metadata_root = metadata_root.replace("artix7", "kintex7")
		xraydb_root = xraydb_root.replace("artix7", "kintex7")
	d = import_device(args.device, xraydb_root, metadata_root)
	# Import tile types
	seen_tiletypes = set()
	tile_types = []
	tile_type_index = {}
	timing = NextpnrTimingData()
	for tile in d.tiles:
		if tile.tile_type() not in seen_tiletypes:
			ntt = NextpnrTileType(d, tile, timing)
			ntt.index = len(tile_types)
			seen_tiletypes.add(tile.tile_type())
			tile_type_index[tile.tile_type()] = len(tile_types)
			tile_types.append(ntt)
			if ntt.cell_timing is not None:
				timing.add_tile(ntt.cell_timing)
	timing.sort_tiles()

	# Import tile instances
	tile_insts = []
	for y in range(d.height):
		for x in range(d.width):
			t = d.tiles_by_xy[x, y]
			nti = NextpnrTileInst(index=len(tile_insts), name=t.name,
				tile_type=tile_type_index[t.tile_type()])
			for s in t.sites():
				nsi = NextpnrSiteInst(name=s.name, package_pin="." if s.package_pin is None else s.package_pin,
					site_xy=s.grid_xy, rel_xy=s.rel_xy(), inter_xy=t.interconn_xy)
				nti.sites.append(nsi)
			nti.tilewire_to_node = [-1] * tile_types[nti.tile_type].tile_wire_count
			tile_insts.append(nti)

	# Begin writing bba
	with open(args.bba, "w") as bbaf:
		bba = BBAWriter(bbaf)
		bba.pre('#include "nextpnr.h"')
		bba.pre('NEXTPNR_NAMESPACE_BEGIN')
		bba.post('NEXTPNR_NAMESPACE_END')
		bba.push('chipdb_blob')
		bba.offset32()
		bba.ref('chip_info', 'chip_info')
		bba.label('extra_constid_strs')
		for i in range(constid.num_base_ids, len(constid.constids)):
			bba.str(constid.constids[i])
		bba.align()
		bba.label('extra_constids')
		bba.u32(constid.num_base_ids)
		bba.u32(len(constid.constids) - constid.num_base_ids)
		bba.ref('extra_constid_strs')
		print("Exporting tile and site type data...")
		for tt in tile_types:
			# List of wires on bels in tile
			for bel in tt.bels:
				bba.label('t{}b{}_wires'.format(tt.index, bel.index))
				for bw in bel.belports:
					bba.u32(bw.name) # port name
					bba.u32(bw.port_type) # port type
					bba.u32(bw.wire) # index of connected tile wire
			# List of uphill pips, downhill pips and bel ports on wires in tile
			for w in tt.wires:
				bba.label('t{}w{}_uh'.format(tt.index, w.index))
				for uh in w.pips_uh:
					bba.u32(uh) # index of uphill pip
				bba.label('t{}w{}_dh'.format(tt.index, w.index))
				for dh in w.pips_dh:
					bba.u32(dh) # index of uphill pip
				bba.label('t{}w{}_bels'.format(tt.index, w.index))
				for bp in w.belpins:
					bba.u32(bp.bel) # index of bel in tile
					bba.u32(bp.port) # bel port constid
			# Bel data for tiletype
			bba.label('t{}_bels'.format(tt.index))
			for b in tt.bels:
				bba.u32(b.name) # name constid
				bba.u32(b.bel_type) # type (compatible type for nextpnr) constid
				bba.u32(b.native_type) # native type (original type in RapidWright) constid
				timing_inst_idx = -1
				if tt.type in timing.tile_type_to_tile_index:
					ttmg = timing.tiles[timing.tile_type_to_tile_index[tt.type]]
					bel_name_inst = constid.make(b.site_type + "/" + constid.constids[b.name])
					site_inst = constid.make(b.site_type)
					if bel_name_inst in ttmg.instance_name_to_index:
						timing_inst_idx = ttmg.instance_name_to_index[bel_name_inst]
					elif site_inst in ttmg.instance_name_to_index:
						timing_inst_idx = ttmg.instance_name_to_index[site_inst]
				bba.u32(timing_inst_idx) # timing instance index
				bba.u32(len(b.belports)) # number of bel port wires
				bba.ref("t{}b{}_wires".format(tt.index, b.index)) # ref to list of bel wires
				bba.u16(b.z) # bel z position
				bba.u16(b.site) # bel site index in tile
				bba.u16(b.site_variant) # bel site variant index
				bba.u16(b.is_routing) # 1 if bel is a routing bel
			# Wire data for tiletype
			bba.label('t{}_wires'.format(tt.index))
			for w in tt.wires:
				bba.u32(w.name) # name constid
				bba.u32(len(w.pips_uh)) # number of uphill pips
				bba.u32(len(w.pips_dh)) # number of downhill pips
				bba.u32(w.timing_class) # timing class index
				bba.ref("t{}w{}_uh".format(tt.index, w.index)) # ref to list of uphill pip indices
				bba.ref("t{}w{}_dh".format(tt.index, w.index)) # ref to list of downhill pip indices
				bba.u32(len(w.belpins)) # number of bel pins on wire
				bba.ref("t{}w{}_bels".format(tt.index, w.index)) # ref to list of bel pins
				bba.u16(w.site if w.is_site else -1) # wire site index in tile if a site wire, else -1 if a tile wire
				bba.u16(0) # padding
				bba.u32(w.intent) # wire intent constid
			# Pip data for tiletype
			bba.label('t{}_pips'.format(tt.index))
			for p in tt.pips:
				bba.u32(p.from_wire) # src tile wire index
				bba.u32(p.to_wire) # dst tile wire index
				bba.u32(p.timing_class) # pip timing class
				bba.u16(0) # padding
				bba.u16(p.pip_type.value)
				bba.u32(p.bel) # bel name constid for site pips
				bba.u32(p.extra_data) # misc extra data for pseudo-pips (e.g lut permutation info)
				bba.u16(p.site) # site index in tile for site pips
				bba.u16(p.site_variant) # site variant index for site pips
		# Per-tile-type data including references to the above lists of objects
		bba.label("tiletype_data")
		for tt in tile_types:
			bba.u32(tt.type) # tile type constid
			bba.u32(len(tt.bels)) # number of bels
			bba.ref("t{}_bels".format(tt.index)) # ref to list of bels
			bba.u32(len(tt.wires)) # number of wires
			bba.ref("t{}_wires".format(tt.index)) # ref to list of wires
			bba.u32(len(tt.pips)) # number of pips
			bba.ref("t{}_pips".format(tt.index)) # ref to list of pips
			bba.u32(timing.tile_type_to_tile_index[tt.type] if tt.type in timing.tile_type_to_tile_index else -1) # tile cell timing data index
		print("Exporting nodes...")
		seen_nodes = set()
		curr = 0
		total = len(d.tiles)
		node_wire_count = []
		node_intent = []
		for row in range(d.height):
			gnd_nodes = []
			vcc_nodes = []
			seen_gnd_nodes = set()
			seen_vcc_nodes = set()
			for col in range(d.width):
				t = d.tiles_by_xy[col, row]
				tt = t.tile_type()
				curr += 1
				for w in t.wires():
					n = w.node()
					uid = n.unique_index()
					if uid in seen_nodes:
						continue
					seen_nodes.add(uid)
					if n.is_gnd() and tt not in ("BRAM_INT_INTERFACE_L", "BRAM_INT_INTERFACE_R", "RCLK_INT_L", "RCLK_INT_R"):
						if uid not in seen_gnd_nodes:
							gnd_nodes.append(n)
							seen_gnd_nodes.add(uid)
						continue
					if n.is_vcc() and tt not in ("BRAM_INT_INTERFACE_L", "BRAM_INT_INTERFACE_R"):
						if uid not in seen_vcc_nodes:
							vcc_nodes.append(n)
							seen_vcc_nodes.add(uid)
					# Nodes only containing 1 wire are just part of the tile and don't need
					# an explicit data structure wasting memory
					if len(n.wires) > 1:
						# List of tile wires in node
						bba.label("n{}_tw".format(len(node_wire_count)))
						# Add interconnect tiles first for better delay estimates in nextpnr
						for j in range(2):
							for w in n.wires:
								if (w.tile.tile_type() in ("INT", "INT_L", "INT_R")) != (j == 0):
									continue
								tileidx = w.tile.y * d.width + w.tile.x
								bba.u32(tileidx) # tile index
								bba.u32(w.index) # wire index in tile
								tile_insts[tileidx].tilewire_to_node[w.index] = len(node_wire_count)
						node_intent.append(constid.make(n.wires[0].intent()))
						node_wire_count.append(len(n.wires))
			# Connect up row and column ground nodes
			for i in range(2):
				wire_count = 0
				bba.label("n{}_tw".format(len(node_wire_count)))
				for n in (vcc_nodes if i == 1 else gnd_nodes):
					for w in n.wires:
						tileidx = w.tile.y * d.width + w.tile.x
						bba.u32(tileidx) # tile index
						bba.u32(w.index) # wire index in tile
						tile_insts[tileidx].tilewire_to_node[w.index] = len(node_wire_count)
						wire_count += 1
				for col in range(d.width):
					t = d.tiles_by_xy[col, row]
					tileidx = row * d.width + col
					bba.u32(tileidx)
					wire_idx = tile_types[tile_insts[tileidx].tile_type].row_vcc_wire_index if i == 1 else tile_types[tile_insts[tileidx].tile_type].row_gnd_wire_index
					bba.u32(wire_idx)
					tile_insts[tileidx].tilewire_to_node[wire_idx] = len(node_wire_count)
					wire_count += 1
				node_wire_count.append(wire_count)
				node_intent.append(constid.make("PSEUDO_VCC" if i == 1 else "PSEUDO_GND"))
		# Create the global Vcc and Ground nodes
		for i in range(2):
			wire_count = 0
			bba.label("n{}_tw".format(len(node_wire_count)))
			for row in range(d.height):
				t = d.tiles_by_xy[0, row]
				tileidx = row * d.width
				bba.u32(tileidx)
				wire_idx = tile_types[tile_insts[tileidx].tile_type].global_vcc_wire_index if i == 1 else tile_types[tile_insts[tileidx].tile_type].global_gnd_wire_index
				bba.u32(wire_idx)
				tile_insts[tileidx].tilewire_to_node[wire_idx] = len(node_wire_count)
				wire_count += 1
			node_wire_count.append(wire_count)
			node_intent.append(constid.make("PSEUDO_VCC" if i == 1 else "PSEUDO_GND"))
		print("Exporting tile and site instances...")
		for ti in tile_insts:
			# Mapping from tile wire to node index
			bba.label("ti{}_wire_to_node".format(ti.index))
			for w2n in ti.tilewire_to_node:
				bba.u32(w2n) # global node index
			# List of site instances in a tile
			bba.label("ti{}_sites".format(ti.index))
			for si in ti.sites:
				bba.str(si.name) # site name char*
				bba.str(si.package_pin) # site package pin char*
				bba.u32(si.site_xy[0]) # site X grid coord
				bba.u32(si.site_xy[1]) # site Y grid coord
				bba.u32(si.rel_xy[0]) # in-tile relative X coord
				bba.u32(si.rel_xy[1]) # in-tile relative Y grid coord
				bba.u32(si.inter_xy[0]) # associated interconn tile X
				bba.u32(si.inter_xy[1]) # associated interconn tile Y
		# List of tile instances and associated metadata
		bba.label("tile_insts")
		for ti in tile_insts:
			bba.str(ti.name) # tile name char*
			bba.u32(ti.tile_type) # index into list of tile types
			bba.u32(len(ti.tilewire_to_node)) # length of tilewire-to-node list
			bba.ref("ti{}_wire_to_node".format(ti.index)) # reference to tilewire-to-node list
			bba.u32(len(ti.sites)) # number of sites in tile
			bba.ref("ti{}_sites".format(ti.index)) # reference to list of site data
		# List of nodes
		bba.label("nodes")
		for i in range(len(node_wire_count)):
			bba.u32(node_wire_count[i]) # number of tile wires in node
			bba.u32(node_intent[i]) # intent code constid of node
			bba.ref("n{}_tw".format(i)) # reference to list of tile wires in node, created earlier
		# Wire timing classes
		bba.label("wire_timing_classes")
		for wc, i in sorted(timing.wire_classes.items(), key=lambda e: e[1]):
			bba.u32(wc.r) # resistance
			bba.u32(wc.c) # capacitance
		# Pip timing classes
		bba.label("pip_timing_classes")
		for pc, i in sorted(timing.pip_classes.items(), key=lambda e: e[1]):
			bba.u16(1 if pc.is_buffered else 0) # buffered or not
			bba.u16(0) # padding
			bba.u32(pc.min_delay) # minimum delay
			bba.u32(pc.max_delay) # maximum delay
			bba.u32(pc.r) # resistance
			bba.u32(pc.c) # capacitance
		for i, tmgt in enumerate(timing.tiles):
			for j, it in enumerate(tmgt.instances):
				for k, vt in enumerate(it.variants):
					# Propagation delays
					bba.label("tmgt{}_i{}_v{}_dels".format(i, j, k))
					for td in vt.delays:
						bba.u32(td.from_port) # from port constid
						bba.u32(td.to_port) # to port constid
						bba.u32(td.min_delay) # min comb delay
						bba.u32(td.max_delay) # max comb delay
					# Timing checks
					bba.label("tmgt{}_i{}_v{}_chks".format(i, j, k))
					for tc in vt.checks:
						bba.u32(tc.chktype.value) # timing check type
						bba.u32(tc.sig_port) # signal port constid
						bba.u32(tc.clock_port) # associated clock port constid
						bba.u32(tc.min_value) # min timing check value
						bba.u32(tc.max_value) # max timing check value
				# Instance variants
				bba.label("tmgt_i{}_v{}".format(i, j))
				for k, vt in enumerate(it.variants):
					bba.u32(vt.variant_name) # variant name constid
					bba.u32(len(vt.delays)) # number of delay entries
					bba.u32(len(vt.checks)) # number of check entries
					bba.ref("tmgt{}_i{}_v{}_dels".format(i, j, k)) # ref to list of delay entries
					bba.ref("tmgt{}_i{}_v{}_chks".format(i, j, k)) # ref to list of check entries
			# Instances in tile
			bba.label("tmgt_i{}".format(i))
			for j, it in enumerate(tmgt.instances):
				bba.u32(it.inst_name) # instance name constid
				bba.u32(len(it.variants)) # number of instance variants
				bba.ref("tmgt_i{}_v{}".format(i, j)) # ref to list of inst variants
		# Cell timing tile types
		bba.label("tile_cell_timing")
		for i, tmgt in enumerate(timing.tiles):
			bba.u32(tmgt.tile_type) # tile type name constid
			bba.u32(len(tmgt.instances)) # number of instances in tile
			bba.ref("tmgt_i{}".format(i)) # ref to list of instances
		# Overall timing data
		bba.label("timing")
		bba.u32(len(timing.tiles)) # number of tile types with cell timing info
		bba.u32(len(timing.wire_classes)) # number of wire classes
		bba.u32(len(timing.pip_classes)) # number of pip classes
		bba.ref("tile_cell_timing") # ref to list of cell timing tile types
		bba.ref("wire_timing_classes") # ref to wire class data list
		bba.ref("pip_timing_classes") # ref to pip class data list
		# Main chip info structure
		bba.label("chip_info")
		bba.str(d.name) # device name char*
		bba.str("prjxray") # generator name char*
		bba.u32(1) # version
		bba.u32(d.width) # tile grid width
		bba.u32(d.height) # tile grid height
		bba.u32(len(tile_insts)) # number of tiles
		bba.u32(len(tile_types)) # number of tiletypes
		bba.u32(len(node_wire_count)) # number of nodes
		bba.ref("tiletype_data") # reference to tiletype data list
		bba.ref("tile_insts") # reference to list of tile instances
		bba.ref("nodes") # reference to list of nodes
		bba.ref("extra_constids") # reference to list of constid strings (extra to baked-in ones)
		bba.u32(1) # only one speed grade currently
		bba.ref("timing") # timing data
		bba.pop()
if __name__ == '__main__':
	main()
