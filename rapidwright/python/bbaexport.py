from xilinx_device import *
from bba import BBAWriter
import sys, argparse
import bels, constid
from nextpnr_structs import *

def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--xray", help="Project X-Ray device database path for current family (e.g. ../prjxray-db/artix7)", type=str, required=True)
	parser.add_argument("--metadata", help="nextpnr-xilinx site metadata root", type=str, required=True)
	parser.add_argument("--device", help="name of device to export", type=str, required=True)
	parser.add_argument("--constids", help="name of nextpnr constids file to read", type=str, required=True)
	parser.add_argument("--bba", help="bba file to write", type=str, required=True)
	args = parser.parse_args()
	# Read baked-in constids
	with open(args.constids, "r") as cf:
		constid.read_base(cf)
	# Read and parse X-ray database
	d = import_device(args.device, args.xray, args.metadata)
	# Import tile types
	seen_tiletypes = set()
	tile_types = []
	tile_type_index = {}
	for tile in d.tiles:
		if tile.tile_type() not in seen_tiletypes:
			ntt = NextpnrTileType(d, tile)
			ntt.index = len(tile_types)
			seen_tiletypes.add(tile.tile_type())
			tile_type_index[tile.tile_type()] = len(tile_types)
			tile_types.append(ntt)

	# Import tile instances
	# FIXME: package pins
	tile_insts = []
	for y in range(d.height):
		for x in range(d.width):
			t = d.tiles_by_xy[x, y]
			nti = NextpnrTileInst(index=len(tile_insts), name=t.name,
				tile_type=tile_type_index[t.tile_type()])
			for s in t.sites():
				nsi = NextpnrSiteInst(name=s.name, package_pin="",
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
				bba.u32(b.name)
				bba.u32(b.bel_type)
				bba.u32(b.native_type)
				bba.u32(len(b.belports))
				bba.ref("t{}b{}_wires".format(tt.index, b.index))
				bba.u16(b.z)
				bba.u16(b.site)
				bba.u16(b.site_variant)
				bba.u16(b.is_routing)
if __name__ == '__main__':
	main()
