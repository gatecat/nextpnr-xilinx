from enum import *
import constid
import bels

class NextpnrPipType(Enum):
	TILE_ROUTING = 0
	SITE_ENTRANCE = 1
	SITE_EXIT = 2
	SITE_INTERNAL = 3
	LUT_PERMUTATION = 4
	LUT_ROUTETHRU = 5

class NextpnrBelPin:
	def __init__(self, bel, port):
		self.bel = bel
		self.port = constid.make(port)

class NextpnrWire:
	def __init__(self, name, index, intent):
		self.name = constid.make(name)
		self.index = index
		self.is_site = False
		self.intent = intent
		self.pips_uh = []
		self.pips_dh = []
		self.belpins = []

class NextpnrPip:
	def __init__(self, index, from_wire, to_wire, delay, pip_type):
		self.index = index
		self.from_wire = from_wire
		self.to_wire = to_wire
		self.delay = delay
		self.pip_type = pip_type
		self.bel = -1
		self.extra_data = 0
		self.site = -1
		self.site_variant = -1

class NextpnrBelWire:
	def __init__(self, name, port_type, wire):
		self.name = name
		self.port_type = port_type
		self.wire = wire

class NextpnrBel:
	def __init__(self, name, index, bel_type, native_type, site, site_variant, z, is_routing):
		self.name = constid.make(name)
		self.index = index
		self.bel_type = constid.make(bel_type)
		self.native_type = constid.make(native_type)
		self.site = site
		self.site_variant = site_variant
		self.z = z
		self.is_routing = is_routing
		self.belports = []

class NextpnrSiteInst:
	def __init__(self, name, package_pin, site_xy, rel_xy, inter_xy):
		self.name = name
		self.package_pin = package_pin
		self.site_xy = site_xy
		self.rel_xy = rel_xy
		self.inter_xy = inter_xy

class NextpnrTileInst:
	def __init__(self, index, name, tile_type):
		self.index = index
		self.name = name
		self.tile_type = tile_type
		self.tilewire_to_node = []
		self.sites = []

class NextpnrTileType:
	def __init__(self, device, tile):
		self.type = constid.make(tile.tile_type())
		self.bels = []
		self.wires = []
		self.pips = []
		self.site_wire_to_wire = {}

		for wire in tile.wires():
			idx = len(self.wires)
			self.wires.append(NextpnrWire(name=wire.name(), index=idx, intent=constid.make(wire.intent())))