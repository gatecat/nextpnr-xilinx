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
		self.site = 0
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

def lookup_port_type(t):
	if t == "INPUT":
		return 0
	elif t == "OUTPUT":
		return 1
	elif t == "BIDIR":
		return 2
	else:
		assert False

class NextpnrTileType:
	def sitewire_to_tilewire(self, sw):
		key = str(sw.site.primary.index) + "/" + sw.name()
		if key not in self.sitewire_to_tilewire_idx:
			nw = NextpnrWire(name=sw.name(), index=len(self.wires), intent=constid.make("INTENT_SITE_WIRE"))
			nw.is_site = True
			nw.site = sw.site.primary.index
			if sw.name() == "GND_WIRE":
				nw.intent = constid.make("INTENT_SITE_GND")
			self.wires.append(nw)
			self.sitewire_to_tilewire_idx[key] = nw.index
		return self.sitewire_to_tilewire_idx[key]
	def __init__(self, device, tile):
		self.type = constid.make(tile.tile_type())
		self.bels = []
		self.wires = []
		self.pips = []
		self.sitewire_to_tilewire_idx = {}

		for wire in tile.wires():
			idx = len(self.wires)
			self.wires.append(NextpnrWire(name=wire.name(), index=idx, intent=constid.make(wire.intent())))
		self.tile_wire_count = len(self.wires)
		for s in tile.sites():
			for variant_idx, variant in enumerate(s.available_variants()):
				sv = s.variant(variant)
				for bel in sv.bels():
					self.add_bel(variant_idx, bel)

	def add_bel(self, site_variant_idx, bel):
		site = bel.site
		z = bels.get_bel_z_override(bel, len(self.bels))
		# Overriden z of -1 means we skip this bel
		if z == -1:
			return None
		name = bel.name()
		print(name)
		bt = bel.bel_type()
		prim_st = site.primary.site_type()
		if prim_st in ("IOB33M", "IOB33S"):
			name = site.site_type() + "/" + name
		nb = NextpnrBel(
			name=name, index=len(self.bels), 
			bel_type=bels.get_bel_type_override(bt),
			native_type=bt,
			site=site.primary.index, site_variant=site_variant_idx,
			z=z, is_routing=(bel.bel_class() == "RBEL")
		)
		self.bels.append(nb)
		for pin in bel.pins():
			nport = NextpnrBelWire(
				name=constid.make(pin.name),
				port_type=lookup_port_type(pin.dir()),
				wire=self.sitewire_to_tilewire(pin.site_wire())
			)
			nb.belports.append(nport)
			self.wires[nport.wire].belpins.append(NextpnrBelPin(nb.index, pin.name))
