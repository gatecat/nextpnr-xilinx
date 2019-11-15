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
		self.index = -1
		self.bels = []
		self.wires = []
		self.pips = []
		self.sitewire_to_tilewire_idx = {}
		# Import tile wires
		for wire in tile.wires():
			idx = len(self.wires)
			self.wires.append(NextpnrWire(name=wire.name(), index=idx, intent=constid.make(wire.intent())))
		self.row_gnd_wire_index = len(self.wires)
		self.wires.append(NextpnrWire(name="PSEUDO_GND_WIRE_ROW", index=self.row_gnd_wire_index,
			intent=constid.make("PSEUDO_GND")))
		self.row_vcc_wire_index = len(self.wires)
		self.wires.append(NextpnrWire(name="PSEUDO_VCC_WIRE_ROW", index=self.row_vcc_wire_index,
			intent=constid.make("PSEUDO_VCC")))
		self.global_gnd_wire_index = len(self.wires)
		self.wires.append(NextpnrWire(name="PSEUDO_GND_WIRE_GLBL", index=self.global_gnd_wire_index,
			intent=constid.make("PSEUDO_GND")))
		self.global_vcc_wire_index = len(self.wires)
		self.wires.append(NextpnrWire(name="PSEUDO_VCC_WIRE_GLBL", index=self.global_vcc_wire_index,
			intent=constid.make("PSEUDO_VCC")))
		self.tile_wire_count = len(self.wires)
		# Import sites
		for s in tile.sites():
			seen_pins = set() # set of (pinname, sitewire name)
			for variant_idx, variant in enumerate(s.available_variants()):
				if variant in ("FIFO36E1", ): #unsupported atm
					continue
				sv = s.variant(variant)
				# Import site variant bels
				for bel in sv.bels():
					self.add_bel(variant_idx, bel)
				# Import site variant pins as site IO pips
				for pin in sv.pins():
					pin_key = (pin.name(), pin.site_wire().name())
					if pin_key not in seen_pins:
						self.add_site_io_pip(pin)
						seen_pins.add(pin_key)
				# Import site variant pips
				for spip in sv.pips():
					self.add_site_pip(variant_idx, spip)


		# Import pips
		is_xc7_logic = (tile.tile_type() in ("CLBLL_L", "CLBLL_R", "CLBLM_L", "CLBLM_R"))
		for p in tile.pips():
			# Exclude certain route-through pips that are broken or unsupported
			if p.is_route_thru() and p.src_wire().name().endswith("_CE_INT"):
				continue
			if p.is_route_thru() and is_xc7_logic:
				continue
			if p.is_route_thru() and "TFB" in p.dst_wire().name():
				continue
			if p.src_wire().name().startswith("CLK_BUFG_R_FBG_OUT"):
				continue
			np = self.add_pip(p, False)
			np.extra_data = 1 if p.is_route_thru() else 0
			if p.is_bidi():
				self.add_pip(p, True)
		# Add pseudo-bels driving Vcc and GND
		self.add_pseudo_bel(name="PSEUDO_GND_BEL", bel_type="PSEUDO_GND", pinname="Y", wire_idx=self.global_gnd_wire_index)
		self.add_pseudo_bel(name="PSEUDO_VCC_BEL", bel_type="PSEUDO_VCC", pinname="Y", wire_idx=self.global_vcc_wire_index)
		# Add pseudo global->row Vcc and GND pips
		self.add_pseudo_pip(from_wire=self.global_gnd_wire_index, to_wire=self.row_gnd_wire_index)
		self.add_pseudo_pip(from_wire=self.global_vcc_wire_index, to_wire=self.row_vcc_wire_index)

	def add_bel(self, site_variant_idx, bel):
		site = bel.site
		z = bels.get_bel_z_override(bel, len(self.bels))
		# Overriden z of -1 means we skip this bel
		if z == -1:
			return None
		name = bel.name()
		bt = bel.bel_type()
		prim_st = site.primary.site_type()
		if prim_st in ("IOB33M", "IOB33S", "IOB33"):
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
		return nb

	def add_site_pip(self, site_variant_idx, sp):
		if "LUT" in sp.bel().bel_type():
			return None # ignore site LUT route-throughs
		bel_name = sp.bel().name()
		bel_pin = sp.bel_input()
		if (bel_name == "ADI1MUX" and bel_pin == "BDI1") or \
			(bel_name == "BDI1MUX" and bel_pin == "DI") or \
			(bel_name == "CDI1MUX" and bel_pin == "DI") or \
            (bel_name == "TFBUSED"):
			return None
		np = NextpnrPip(index=len(self.pips),
			from_wire=self.sitewire_to_tilewire(sp.src_wire()),
			to_wire=self.sitewire_to_tilewire(sp.dst_wire()),
			delay=0, pip_type=NextpnrPipType.SITE_INTERNAL
		)
		self.wires[np.from_wire].pips_dh.append(np.index)
		self.wires[np.to_wire].pips_uh.append(np.index)
		np.bel = constid.make(bel_name)
		np.site = sp.site.primary.index
		np.site_variant = site_variant_idx
		np.extra_data = constid.make(bel_pin)
		self.pips.append(np)
		return np

	def add_site_io_pip(self, pin):
		pn = pin.name()
		s = pin.site
		pindir = pin.dir()
		if pin.tile_wire() is None:
			return None # ignore pins that are not connected to a useful tile wire
		if pindir in ("OUTPUT", "BIDIR"):
			if s.primary.site_type() == "IPAD" and pn == "O":
				return None
			np = NextpnrPip(index=len(self.pips),
				from_wire=self.sitewire_to_tilewire(pin.site_wire()),
				to_wire=pin.tile_wire().index,
				delay=0, pip_type=NextpnrPipType.SITE_EXIT
			)
		else:
			if s.site_type() in ("SLICEL", "SLICEM"):
				# Add permuation pseudo-pips for LUT inputs
				swn = pin.site_wire().name()
				if len(swn) == 2 and swn[0] in "ABCDEFGH" and swn[1] in "123456":
					i = int(swn[1])
					for j in range(1, 7):
						if (i == 6) != (j == 6):
							continue # don't allow permutation of input 6
						pp = NextpnrPip(index=len(self.pips),
							from_wire=s.pin(swn[0] + str(j)).tile_wire().index,
							to_wire=self.sitewire_to_tilewire(pin.site_wire()),
							delay=0, pip_type=NextpnrPipType.LUT_PERMUTATION)
						pp.extra_data = ("ABCDEFGH".index(swn[0]) << 8) | ((j - 1) << 4) | (i - 1)
						if s.rel_xy()[0] == 1:
							pp.extra_data |= (4 << 8)
						self.wires[pp.from_wire].pips_dh.append(pp.index)
						self.wires[pp.to_wire].pips_uh.append(pp.index)
						self.pips.append(pp)
					return None
			np = NextpnrPip(index=len(self.pips),
				from_wire=pin.tile_wire().index,
				to_wire=self.sitewire_to_tilewire(pin.site_wire()),
				delay=0, pip_type=NextpnrPipType.SITE_ENTRANCE
			)
		self.wires[np.from_wire].pips_dh.append(np.index)
		self.wires[np.to_wire].pips_uh.append(np.index)
		self.pips.append(np)
		return np

	def add_pip(self, p, reverse):
		np = NextpnrPip(index=len(self.pips),
			from_wire=(p.dst_wire() if reverse else p.src_wire()).index,
			to_wire=(p.src_wire() if reverse else p.dst_wire()).index,
			delay=0, pip_type=NextpnrPipType.TILE_ROUTING
		)
		self.wires[np.from_wire].pips_dh.append(np.index)
		self.wires[np.to_wire].pips_uh.append(np.index)
		self.pips.append(np)
		return np

	def add_pseudo_pip(self, from_wire, to_wire):
		np = NextpnrPip(index=len(self.pips),
			from_wire=from_wire, to_wire=to_wire,
			delay=0, pip_type=NextpnrPipType.TILE_ROUTING
		)
		self.wires[np.from_wire].pips_dh.append(np.index)
		self.wires[np.to_wire].pips_uh.append(np.index)
		self.pips.append(np)
		return np

	def add_pseudo_bel(self, name, bel_type, pinname, wire_idx):
		nb = NextpnrBel(name=name, index=len(self.bels),
			bel_type=bel_type, native_type=bel_type,
			site=-1, site_variant=0, z=len(self.bels), is_routing=False
		)
		nb.belports.append(NextpnrBelWire(name=constid.make(pinname), port_type=1, wire=wire_idx))
		self.wires[wire_idx].belpins.append(NextpnrBelPin(bel=nb.index, port=pinname))
		self.bels.append(nb)
