
package dev.fpga.rapidwright;

import com.xilinx.rapidwright.design.Design;
import com.xilinx.rapidwright.design.SiteInst;
import com.xilinx.rapidwright.device.PartNameTools;
import com.xilinx.rapidwright.device.*;
import com.xilinx.rapidwright.edif.*;
import com.xilinx.rapidwright.util.Utils;
import com.xilinx.rapidwright.util.RapidWright;
import com.xilinx.rapidwright.timing.*;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.util.*;

public class bbaexport {

    static boolean xc7_flag = false;

    public static String sitePinToGlobalWire(HashSet<Node> discoveredWires, Device d, Site s, String pinname) {
        String tw = s.getTileWireNameFromPinName(pinname);
        Wire w = d.getWire(s.getTile().getName() + "/" + tw);
        Node wn = w.getNode();
        discoveredWires.add(wn);
        return wn.getTile().getName() + "/" + wn.getWireName();
    }

    enum NextpnrPipType {
        TILE_ROUTING,
        SITE_ENTRANCE,
        SITE_EXIT,
        SITE_INTERNAL,
        LUT_PERMUTATION,
        LUT_ROUTETHRU,
        CONST_DRIVER,
    }

    static class NextpnrBelPin {
        public NextpnrBelPin(int bel, String port) {
            this.bel = bel;
            this.port = makeConstId(port);
        }
        public int bel;
        public int port;
    }

    static class NextpnrWire {

        public int name;
        public int index;

        public NextpnrWire(String name, int index, int intent) {
            this.name = makeConstId(name);
            this.index = index;
            this.is_site = false;
            this.intent = intent;
            this.pips_uh = new ArrayList<>();
            this.pips_dh = new ArrayList<>();
            this.belpins = new ArrayList<>();
        }

        public boolean is_site;
        public int site;
        public int intent;
        public ArrayList<Integer> pips_uh, pips_dh;
        public ArrayList<NextpnrBelPin> belpins;
    }

    static class NextpnrPip {

        public NextpnrPip(int index, int from, int to, int tmg_cls, NextpnrPipType type) {
            this.index = index;
            this.from = from;
            this.to = to;
            this.tmg_cls = tmg_cls;
            this.type = type;
        }

        public int index;
        public int from, to;
        public int tmg_cls;
        public NextpnrPipType type;

        public int bel = -1;
        public int extra_data = 0;
        public int site = -1;
        public int siteVariant = -1;
    }

    static class NextpnrBelWire {
        public int name;
        public int port_type;
        public int wire;
    }

    static class NextpnrBel {

        public NextpnrBel(String name, int index, String type, String nativeType, int site, int siteVariant, int z, int isRouting) {
            this.name = makeConstId(name);
            this.index = index;
            this.type = makeConstId(type);
            this.nativeType = makeConstId(type);
            this.site = site;
            this.siteVariant = siteVariant;
            this.z = z;
            this.isRouting = isRouting;
            this.belports = new ArrayList<>();
        }
        public int name;
        public int index;
        public int type, nativeType;
        public int site, siteVariant;
        public int isRouting;
        public int z;
        public ArrayList<NextpnrBelWire> belports;
    }

    static class NextpnrPropDelay {
        public NextpnrPropDelay(String from_port, String to_port, int min_delay, int max_delay) {
            this.from_port = makeConstId(from_port);
            this.to_port = makeConstId(to_port);
            this.min_delay = min_delay;
            this.max_delay = max_delay;
        }
        public int from_port;
        public int to_port;
        public int min_delay;
        public int max_delay;
    }

    enum NextpnrTmgChkType {
        TIMING_CHECK_SETUP,
        TIMING_CHECK_HOLD,
        TIMING_CHECK_WIDTHd,
    }

    static class NextpnrTimingCheck {
        public NextpnrTimingCheck(NextpnrTmgChkType chktype, String sig_port, String clock_port, int min_value, int max_value) {
            this.chktype = chktype;
            this.sig_port = makeConstId(sig_port);
            this.clock_port = makeConstId(clock_port);
            this.min_value = min_value;
            this.max_value = max_value;
        }
        public NextpnrTmgChkType chktype;
        public int sig_port;
        public int clock_port;
        public int min_value;
        public int max_value;
    }

    static class NextpnrCellTiming {
        public NextpnrCellTiming(String variant_name) {
            this.variant_name = makeConstId(variant_name);
            this.delays = new ArrayList<>();
            this.checks = new ArrayList<>();
        }
        public int variant_name;
        public ArrayList<NextpnrPropDelay> delays;
        public ArrayList<NextpnrTimingCheck> checks;
        public void sort() {
            Collections.sort(delays, new Comparator<NextpnrPropDelay>() {
                @Override
                public int compare(NextpnrPropDelay o1, NextpnrPropDelay o2) {
                    int tc = Integer.compare(o1.to_port, o2.to_port);
                    if (tc != 0)
                        return tc;
                    return Integer.compare(o1.from_port, o2.from_port);
                }
            });
            Collections.sort(checks, new Comparator<NextpnrTimingCheck>() {
                @Override
                public int compare(NextpnrTimingCheck o1, NextpnrTimingCheck o2) {
                    int tc = Integer.compare(o1.sig_port, o2.sig_port);
                    if (tc != 0)
                        return tc;
                    return Integer.compare(o1.clock_port, o2.clock_port);
                }
            });
        }
    }

    static class NextpnrInstanceTiming {
        public NextpnrInstanceTiming(String inst_name) {
            this.inst_name = makeConstId(inst_name);
            this.variants = new ArrayList<>();
        }

        public void sort() {
            Collections.sort(variants, new Comparator<NextpnrCellTiming>() {
                @Override
                public int compare(NextpnrCellTiming o1, NextpnrCellTiming o2) {
                    return Integer.compare(o1.variant_name, o2.variant_name);
                }
            });
        }

        public int inst_name;
        public ArrayList<NextpnrCellTiming> variants;
    }

    static class NextpnrTileCellTiming {
        public NextpnrTileCellTiming(String tile_type) {
            this.tile_type = makeConstId(tile_type);
            this.instances = new ArrayList<>();
        }

        public void sort() {
            Collections.sort(instances, new Comparator<NextpnrInstanceTiming>() {
                @Override
                public int compare(NextpnrInstanceTiming o1, NextpnrInstanceTiming o2) {
                    return Integer.compare(o1.inst_name, o2.inst_name);
                }
            });
        }

        public int tile_type;
        public ArrayList<NextpnrInstanceTiming> instances;
    }

    private static ArrayList<String> constIds = new ArrayList<>();
    private static HashMap<String, Integer> knownConstIds = new HashMap<>();

    private static ArrayList<Integer> pipDelays = new ArrayList<>();
    private static HashMap<Integer, Integer> knownPipDelays = new HashMap<>();

    private static int get_pip_timing_class(int delay_ps) {
        if (knownPipDelays.containsKey(delay_ps))
            return knownPipDelays.get(delay_ps);
        int index = pipDelays.size();
        knownPipDelays.put(delay_ps, index);
        pipDelays.add(delay_ps);
        return index;
    }

    static class NextpnrTileType {
        public int index;
        public int type;
        public ArrayList<NextpnrBel> bels;
        public ArrayList<NextpnrWire> wires;
        public ArrayList<NextpnrPip> pips;

        public int tile_wire_count = 0; // excluding site wires
        public int row_gnd_wire_index, row_vcc_wire_index, global_gnd_wire_index, global_vcc_wire_index;
        public HashMap<String, Integer> siteWiresToWireIndex;

        private int siteWireToWire(Site s, String wire) {
            String key = s.getSiteTypeEnum().toString() + s.getSiteIndexInTile() + "/" + wire;
            if (siteWiresToWireIndex.containsKey(key))
                return siteWiresToWireIndex.get(key);
            NextpnrWire nw = new NextpnrWire(wire, wires.size(), makeConstId("SITE_WIRE"));
            nw.is_site = true;
            nw.site = s.getSiteIndexInTile();
            if (wire.equals("GND_WIRE"))
                nw.intent = makeConstId("INTENT_SITE_GND");
            else
                nw.intent = makeConstId("INTENT_SITE_WIRE");
            wires.add(nw);
            siteWiresToWireIndex.put(key, nw.index);
            return nw.index;
        }

        private NextpnrBel addBel(SiteInst s, int siteVariant, BEL b) {
            if (b.getBELClass() == BELClass.PORT)
                return null;
            int z = getBelZoverride(s.getTile(), s.getSite(), b);
            if (z == -1)
                return null;

            String name = b.getName();

            if (s.getPrimarySiteTypeEnum() == SiteTypeEnum.IOB33M || s.getPrimarySiteTypeEnum() == SiteTypeEnum.IOB33S) {
                name = s.getSiteTypeEnum().toString() + "/" + name;
            }

            NextpnrBel nb = new NextpnrBel(name,
                    bels.size(), getBelTypeOverride(b.getBELType()), b.getBELType(), s.getSite().getSiteIndexInTile(), siteVariant, z,
                    (b.getBELClass() == BELClass.RBEL) ? 1 : 0);
            bels.add(nb);
            belsInTile.put(s.getTile(), belsInTile.getOrDefault(s.getTile(), 0) + 1);

            for (BELPin bp : b.getPins()) {
                NextpnrBelWire nport = new NextpnrBelWire();
                nport.port_type = bp.isBidir() ? 2 : (bp.isOutput() ? 1 : 0);
                nport.name = makeConstId(bp.getName());
                nport.wire = siteWireToWire(s.getSite(), bp.getSiteWireName());
                nb.belports.add(nport);

                wires.get(nport.wire).belpins.add(new NextpnrBelPin(nb.index, bp.getName()));

                // For HARD0 bels, create a pip from the tile-wide ground pseudo-wire to the HARDGND output
                // for ease of constant routing in nextpnr
                if (b.getBELType().equals("HARD0") && bp.getName().equals("0")) {
                    NextpnrPip np = new NextpnrPip(pips.size(), row_gnd_wire_index, siteWireToWire(s.getSite(), bp.getSiteWireName()),
                            0, NextpnrPipType.SITE_ENTRANCE);
                    wires.get(np.from).pips_dh.add(np.index);
                    wires.get(np.to).pips_uh.add(np.index);
                    np.bel = makeConstId(b.getName());
                    np.site = s.getSite().getSiteIndexInTile();
                    np.siteVariant = siteVariant;
                    pips.add(np);
                }
            }

            return nb;
        }

        private NextpnrPip addSitePIP(SiteInst s, int siteVariant, SitePIP sp) {
            if (sp.getBEL().getBELType().contains("LUT"))
                return null; // Ignore LUT route-through pips for now
            if ((sp.getBELName().equals("ADI1MUX") && sp.getInputPinName().equals("BDI1")) ||
                (sp.getBELName().equals("BDI1MUX") && sp.getInputPinName().equals("DI")) ||
                (sp.getBELName().equals("CDI1MUX") && sp.getInputPinName().equals("DI")))
                return null;
            if (sp.getBELName().startsWith("TFBUSED"))
                return null;
            NextpnrPip np = new NextpnrPip(pips.size(), siteWireToWire(s.getSite(), sp.getInputPin().getSiteWireName()),
                    siteWireToWire(s.getSite(), sp.getOutputPin().getSiteWireName()), 0, NextpnrPipType.SITE_INTERNAL);
            wires.get(np.from).pips_dh.add(np.index);
            wires.get(np.to).pips_uh.add(np.index);

            np.bel = makeConstId(sp.getBELName());
            np.site = s.getSite().getSiteIndexInTile();
            np.siteVariant = siteVariant;
            np.extra_data = makeConstId(sp.getInputPinName());

            pips.add(np);
            return np;
        }

        private NextpnrPip addSiteIOPIP(Device d, Site s, SiteInst si, BELPin bp) {
            NextpnrPip np;
            String sitePinName = bp.getConnectedSitePinName();
            if (bp.isOutput() || bp.isBidir()) {
                if (si.getSiteTypeEnum() == SiteTypeEnum.IPAD && sitePinName.equals("O"))
                    return null;
                String sitePin = si.getPrimarySitePinName(sitePinName);
                if (s.getSiteTypeEnum() == si.getSiteTypeEnum())
                    sitePin = sitePinName;
                //System.out.println(s.getName() + " " + sitePinName + " " + si.getPrimarySitePinName(sitePinName) + " " + sitePin + " " + si.getSiteTypeEnum() + " " + s.getTileWireNameFromPinName(sitePin));
                if (s.getSiteTypeEnum() == SiteTypeEnum.SLICEL && sitePinName.equals("A_O")) {
                    addPseudoPIP(row_gnd_wire_index, s.getTile().getWireIndex(s.getTileWireNameFromPinName(sitePin)), NextpnrPipType.CONST_DRIVER);
                }
                np = new NextpnrPip(pips.size(), siteWireToWire(s, bp.getSiteWireName()), s.getTile().getWireIndex(s.getTileWireNameFromPinName(sitePin)),
                        0, NextpnrPipType.SITE_EXIT);
            } else {
                if (((s.getSiteTypeEnum() == SiteTypeEnum.SLICEL || s.getSiteTypeEnum() == SiteTypeEnum.SLICEM))) {
                    // Permutation pseudo-pips for LUT pins

                    String pn = bp.getSiteWireName();
                    if (pn.length() == 2 && "ABCDEFGH".contains(pn.substring(0, 1)) && "123456".contains(pn.substring(1, 2))) {
                        // No permutation for 6 ATM
                        int i = "123456".indexOf(pn.substring(1, 2)) + 1;
                        for (int j = 1; j <= 6; j++) {
                            if ((i == 6) != (j == 6))
                                continue; // don't allow permutation of input 6
                            NextpnrPip pp = new NextpnrPip(pips.size(), s.getTile().getWireIndex(s.getTileWireNameFromPinName(pn.substring(0, 1) + j)), siteWireToWire(s, bp.getSiteWireName()),
                                    0, NextpnrPipType.LUT_PERMUTATION);
                            // extra data: eigth[3:0]; from[3:0]; to[3:0]
                            pp.extra_data = ("ABCDEFGH".indexOf(pn.substring(0,1)) << 8) | ((j - 1) << 4) | (i - 1);
                            if (xc7_flag) {
                                boolean is_upper_site = false;
                                for (Site s2 : s.getTile().getSites()) {
                                    if (s2.getInstanceX() < s.getInstanceX()) {
                                        is_upper_site = true;
                                        break;
                                    }
                                }
                                if (is_upper_site)
                                    pp.extra_data |= (4 << 8);
                            }
                            wires.get(pp.from).pips_dh.add(pp.index);
                            wires.get(pp.to).pips_uh.add(pp.index);
                            pips.add(pp);
                        }

                        return null;
                    }

                }
                String sitePin = si.getPrimarySitePinName(sitePinName);
                if (s.getSiteTypeEnum() == si.getSiteTypeEnum())
                    sitePin = sitePinName;
                //System.out.println(s.getName() + " " + sitePinName + " " + si.getPrimarySitePinName(sitePinName) + " " + sitePin + " " + si.getSiteTypeEnum() + " " + s.getTileWireNameFromPinName(sitePin));
                np = new NextpnrPip(pips.size(), s.getTile().getWireIndex(s.getTileWireNameFromPinName(sitePin)), siteWireToWire(s, bp.getSiteWireName()),
                        0, NextpnrPipType.SITE_ENTRANCE);

            }
            wires.get(np.from).pips_dh.add(np.index);
            wires.get(np.to).pips_uh.add(np.index);
            pips.add(np);
            return np;
        }

        private NextpnrPip addPIP(TimingModel m, PIP p, boolean reverse) {


            // Note this is a first-pass model. It assumes that all pips are buferred and all pips have the same
            // delay regardless of location. It is to guide the nextpnr router rather than give sign-off quality
            // STA.
            TimingGroup tg = new TimingGroup(m);
            tg.add(p.getStartNode(), p.getStartWire().getIntentCode());
            tg.add(p);
            tg.add(p.getEndNode(), p.getEndWire().getIntentCode());
            var delay = m.calcDelay(tg);
            int tmg_cls = get_pip_timing_class((int)(delay));

            NextpnrPip np = new NextpnrPip(pips.size(), reverse ?  p.getEndWireIndex() : p.getStartWireIndex(), reverse ?  p.getStartWireIndex() : p.getEndWireIndex(), tmg_cls, NextpnrPipType.TILE_ROUTING);
            wires.get(np.from).pips_dh.add(np.index);
            wires.get(np.to).pips_uh.add(np.index);
            pips.add(np);
            return np;
        }

        private NextpnrPip addPseudoPIP(int from, int to) {
            return addPseudoPIP(from, to,  NextpnrPipType.TILE_ROUTING);
        }

        private NextpnrPip addPseudoPIP(int from, int to, NextpnrPipType t) {
            NextpnrPip np = new NextpnrPip(pips.size(), from, to, 0, t);
            wires.get(np.from).pips_dh.add(np.index);
            wires.get(np.to).pips_uh.add(np.index);
            pips.add(np);
            return np;
        }

        private void addPsuedoBel(Tile t, String name, String type, String pinName, int wire) {
            NextpnrBel nb = new NextpnrBel(name, bels.size(), type, type, -1, 0, bels.size(), 0);
            NextpnrBelWire port = new NextpnrBelWire();
            port.name = makeConstId(pinName);
            port.port_type = 1;
            port.wire = wire;
            nb.belports.add(port);
            wires.get(wire).belpins.add(new NextpnrBelPin(nb.index, pinName));
            bels.add(nb);
        }

        public void importTile(Device d, Design des, TimingModel tmg, Tile t) {


            type = makeConstId(t.getTileTypeEnum().name());
            bels = new ArrayList<>();
            wires = new ArrayList<>();
            pips = new ArrayList<>();
            siteWiresToWireIndex = new HashMap<>();

            for (String wn : t.getWireNames()) {
                int index = wires.size();
                Wire w = new Wire(t, wn);
                wires.add(new NextpnrWire(wn, index, makeConstId(w.getIntentCode().toString())));
            }

            // Add a special wires
            row_gnd_wire_index = wires.size();
            wires.add(new NextpnrWire("PSEUDO_GND_WIRE_ROW", row_gnd_wire_index, makeConstId("PSEUDO_GND")));
            row_vcc_wire_index = wires.size();
            wires.add(new NextpnrWire("PSEUDO_VCC_WIRE_ROW", row_vcc_wire_index, makeConstId("PSEUDO_VCC")));
            global_gnd_wire_index = wires.size();
            wires.add(new NextpnrWire("PSEUDO_GND_WIRE_GLBL", global_gnd_wire_index, makeConstId("PSEUDO_GND")));
            global_vcc_wire_index = wires.size();
            wires.add(new NextpnrWire("PSEUDO_VCC_WIRE_GLBL", global_vcc_wire_index, makeConstId("PSEUDO_VCC")));

            tile_wire_count = wires.size();
            int autoidx = 0;
            for (Site s : t.getSites()) {
                HashSet<BELPin> sitePins = new HashSet<>();
                ArrayList<SiteTypeEnum> variants = new ArrayList<>();
                variants.add(s.getSiteTypeEnum());
                variants.addAll(Arrays.asList(s.getAlternateSiteTypeEnums()));

                for (int variant = 0; variant < variants.size(); variant++) {

                    SiteInst si = new SiteInst(t.getName() + "_" + (autoidx++), des, variants.get(variant), s);

                    HashSet<SitePIP> sitePips = new HashSet<>();
                    for (BEL b : si.getBELs()) {
                        addBel(si, variant, b);
                        for (BELPin bp : b.getPins()) {
                            String sitePin = bp.getConnectedSitePinName();
                            if (sitePin != null && !sitePins.contains(bp)) {
                                sitePins.add(bp);
                                addSiteIOPIP(d, s, si, bp);
                            }
                            sitePips.addAll(bp.getSitePIPs());
                        }

                    }
                    for (SitePIP sp : sitePips)
                        addSitePIP(si, variant, sp);

                    si.unPlace();
                }
            }
            TileTypeEnum tt = t.getTileTypeEnum();
            boolean isLogic = (tt == TileTypeEnum.CLEM || tt == TileTypeEnum.CLEM_R || tt == TileTypeEnum.CLEL_L || tt == TileTypeEnum.CLEL_R);
            boolean isxc7Logic = (tt == TileTypeEnum.CLBLL_L || tt == TileTypeEnum.CLBLL_R || tt == TileTypeEnum.CLBLM_L || tt == TileTypeEnum.CLBLM_R);
            for (PIP p : t.getPIPs()) {
                if (p.isRouteThru() && p.getStartWireName().endsWith("_CE_INT"))
                    continue; // these route through pips seem to cause antenna issues
                if (p.isRouteThru() && isxc7Logic)
                    continue; // fixme
                if (xc7_flag && p.isRouteThru() && p.getEndWireName().contains("TFB"))
                    continue;
                if (xc7_flag && p.getStartWireName().startsWith("CLK_BUFG_R_FBG_OUT"))
                    continue;
                NextpnrPip np = addPIP(tmg, p, false);
                if (p.isRouteThru() && isLogic) {
                    np.type = NextpnrPipType.LUT_ROUTETHRU;
                    // extra data: eigth[3:0]; from[3:0]; to[3:0]
                    String fromPin = p.getStartWireName(), toPin = p.getEndWireName();
                    fromPin = fromPin.substring(fromPin.length() - 2);
                    np.extra_data = ("ABCDEFGH".indexOf(fromPin.substring(0,1)) << 8) | ("123456".indexOf(fromPin.substring(1, 2)) << 4) | (toPin.endsWith("MUX") ? 1 : 0);
                } else {
                    np.extra_data = p.isRouteThru() ? 1 : 0;
                }
                if (p.isBidirectional())
                    addPIP(tmg, p, true);
            }
            // Add pseudo-bels driving Vcc and GND
            addPsuedoBel(t, "PSEUDO_GND_BEL", "PSEUDO_GND", "Y", global_gnd_wire_index);
            addPsuedoBel(t, "PSEUDO_VCC_BEL", "PSEUDO_VCC", "Y", global_vcc_wire_index);
            // Add pseudo global->row Vcc and GND pips
            addPseudoPIP(global_gnd_wire_index, row_gnd_wire_index);
            addPseudoPIP(global_vcc_wire_index, row_vcc_wire_index);
        }
    }

    static class NextpnrSiteInst {
        public String name;
        public String packagePin;
        public int site_x, site_y;
        public int rel_x, rel_y;
        public int inter_x, inter_y;
    }
    static class NextpnrTileInst {
        public int index;
        public String name;
        public int type;

        public Integer[] tilewire_to_node;

        public ArrayList<NextpnrSiteInst> sites;
    }

    private static String getBelTypeOverride(String type) {
        if (type.endsWith("6LUT") || type.equals("LUT_OR_MEM6") || type.equals("LUT6"))
            return "SLICE_LUTX";
        if (type.endsWith("5LUT") || type.equals("LUT_OR_MEM5") || type.equals("LUT5"))
            return "SLICE_LUTX";
        if (type.length() == 4 && type.endsWith("FF2"))
            return "SLICE_FFX";
        if (type.length() == 3 && type.endsWith("FF"))
            return "SLICE_FFX";
        if (type.equals("FF_INIT") || type.equals("REG_INIT"))
            return "SLICE_FFX";
        if (!xc7_flag) {
            String[] iobParts = {"_PAD_", "_PULL_", "_IBUFCTRL_", "_INBUF_", "_OUTBUF_"};
            for (String p : iobParts) {
                if (type.contains(p))
                    return "IOB_" + p.replace("_", "");
            }
        }

        String[] iolParts = {"COMBUF_", "IDDR_", "IPFF_", "OPFF_", "OPTFF_", "TFF_"};
        for (String p : iolParts) {
            if (type.startsWith(p))
                return "IOL_" + p.replace("_", "");
        }

        if (type.endsWith("_VREF"))
            return "IOB_VREF";

        if (type.endsWith("_DIFFINBUF"))
            return "IOB_DIFFINBUF";

        if (type.startsWith("PSS_ALTO_CORE_PAD_"))
            return "PSS_PAD";

        if (type.startsWith("LAGUNA_RX_REG") || type.startsWith("LAGUNA_TX_REG"))
            return "LAGUNA_REGX";

        if (type.startsWith("BSCAN"))
            return "BSCAN";

        if (type.equals("BUFGCTRL_BUFGCTRL"))
            return "BUFGCTRL";

        if (type.equals("RAMB18E2_U_RAMB18E2") || type.equals("RAMB18E2_L_RAMB18E2"))
            return "RAMB18E2_RAMB18E2";

        return type;
    }


    private static HashMap<Tile, Integer> belsInTile = new HashMap<>();

    private static int makeConstId(String s) {
        if (knownConstIds.containsKey(s))
            return knownConstIds.get(s);
        knownConstIds.put(s, constIds.size());
        constIds.add(s);
        return constIds.size() - 1;
    }

    private static int getBelZoverride(Tile t, Site s, BEL b) {

        if (t.getTileTypeEnum() == TileTypeEnum.BRAM) {
            switch (b.getBELType()) {
                case "RAMBFIFO36E2_RAMBFIFO36E2":
                    return 0;
                case "RAMB36E2_RAMB36E2":
                    return 1;
                case "FIFO36E2_FIFO36E2":
                    return 2;
                case "RAMB18E2_U_RAMB18E2":
                    return 5;
                case "RAMBFIFO18E2_RAMBFIFO18E2":
                    return 8;
                case "RAMB18E2_L_RAMB18E2":
                    return 9;
                case "FIFO18E2_FIFO18E2":
                    return 10;
            }
        } else if (t.getTileTypeEnum() == TileTypeEnum.DSP) {
            boolean is_upper_site = false;
            for (Site s2 : t.getSites()) {
                if (s2.getInstanceY() < s.getInstanceY()) {
                    is_upper_site = true;
                    break;
                }
            }
            int zs = is_upper_site ? (1 << 5) : 0;
            switch (b.getBELType()) {
                case "DSP_PREADD_DATA":
                    return zs | 0;
                case "DSP_PREADD":
                    return zs | 1;
                case "DSP_A_B_DATA":
                    return zs | 2;
                case "DSP_MULTIPLIER":
                    return zs | 3;
                case "DSP_C_DATA":
                    return zs | 4;
                case "DSP_M_DATA":
                    return zs | 5;
                case "DSP_ALU":
                    return zs | 6;
                case "DSP_OUTPUT":
                    return zs | 7;
                default:
                    return -1;
            }
        } else if (t.getTileTypeEnum() == TileTypeEnum.BRAM_L || t.getTileTypeEnum() == TileTypeEnum.BRAM_R) {
            boolean is_top18 = false;
            if (s.getSiteTypeEnum() == SiteTypeEnum.RAMB18E1) {
                is_top18 = true;
            }
            switch(b.getBELType()) {
                case "RAMBFIFO36E1_RAMBFIFO36E1":
                    return 0;
                case "RAMB36E1_RAMB36E1":
                    return 1;
                case "FIFO36E1_FIFO36E1":
                    return 2;
                case "RAMB18E1_RAMB18E1":
                    return is_top18 ? 5 : 9;
                case "FIFO18E1_FIFO18E1":
                    return 10;
            }
        }

        if (b.getSiteTypeEnum() == SiteTypeEnum.SLICEL || b.getSiteTypeEnum() == SiteTypeEnum.SLICEM) {
            if (xc7_flag) {
                boolean is_upper_site = false;
                for (Site s2 : t.getSites()) {
                    if (s2.getInstanceX() < s.getInstanceX()) {
                        is_upper_site = true;
                        break;
                    }
                }
                String subslices = "ABCD";
                String name = b.getName();
                String[] postfixes = {"6LUT", "5LUT", "FF", "5FF"};
                for (int i = 0; i < postfixes.length; i++) {
                    if (name.length() == postfixes[i].length() + 1 && name.substring(1).equals(postfixes[i])) {
                        return (is_upper_site ? 64 : 0) | subslices.indexOf(name.charAt(0)) << 4 | i;
                    }
                }
                switch (name) {
                    case "F7AMUX":
                        return is_upper_site ? 0x47 : 0x07;
                    case "F7BMUX":
                        return is_upper_site ? 0x67 : 0x27;
                    case "F8MUX":
                        return is_upper_site ? 0x48 : 0x08;
                    case "CARRY4":
                        return is_upper_site ? 0x4F : 0x0F;
                }

            } else {
                // LUTs, FFs, and default muxes follow a regular pattern
                // z[6..4] = z-index (A-H)
                // z[3..0] = type
                // Types: [6LUT, 5LUT, FF, FF2, FFMUX1, FFMUX2, OUTMUX, F7MUX, F8MUX, F9MUX, CARRY8, CLKINV, RSTINV, HARD0]
                String subslices = "ABCDEFGH";
                String name = b.getName();
                String[] postfixes = {"6LUT", "5LUT", "FF", "FF2"};
                for (int i = 0; i < postfixes.length; i++) {
                    if (name.length() == postfixes[i].length() + 1 && name.substring(1).equals(postfixes[i])) {
                        return subslices.indexOf(name.charAt(0)) << 4 | i;
                    }
                }
                if (name.startsWith("FFMUX"))
                    return subslices.indexOf(name.charAt(5)) << 4 | (name.charAt(6) == '2' ? 5 : 4);
                if (name.startsWith("OUTMUX"))
                    return subslices.indexOf(name.charAt(6)) << 4 | 6;

                // More special features
                switch (name) {
                    case "F7MUX_AB":
                        return 0x07;
                    case "F7MUX_CD":
                        return 0x27;
                    case "F7MUX_EF":
                        return 0x47;
                    case "F7MUX_GH":
                        return 0x67;
                    case "F8MUX_BOT":
                        return 0x08;
                    case "F8MUX_TOP":
                        return 0x48;
                    case "F9MUX":
                        return 0x09;
                    case "CARRY8":
                        return 0x0A;
                    case "CLK1INV":
                        return 0x0B;
                    case "CLK2INV":
                        return 0x4B;
                    case "LCLKINV":
                        return 0x1B;
                    case "RST_ABCDINV":
                        return 0x0C;
                    case "RST_EFGHINV":
                        return 0x4C;
                    case "HARD0":
                        return 0x0D;
                }
            }


            // Other bels (e.g. extra xc7 routing bels) can be ignored for nextpnr porpoises
            return -1;
        } else {
            return belsInTile.getOrDefault(t, 0);
        }

    }

    public static ArrayList<NextpnrTileType> tileTypes = new ArrayList<>();
    public static HashMap<TileTypeEnum, Integer> tileTypeIndices = new HashMap<>();

    public static ArrayList<NextpnrTileInst> tileInsts = new ArrayList<>();
    public static HashMap<Integer, NextpnrTileInst> tileToTileInst = new HashMap<>();


    public static void main(String[] args) throws IOException {

        if (args.length < 3) {
            System.err.println("Usage: bbaexport <device> <constids.inc> <output.bba>");
            System.err.println("   e.g bbaexport xczu2cg-sbva484-1-e ./rapidwright/constids.inc ./rapidwright/xczu2cg.bba");
            System.err.println("   Use bbasm to convert bba to bin for nextpnr");
            System.exit(1);
        }

        // Device d = Device.getDevice("xczu2cg-sbva484-1-e");

        // Seems like we need to use a Design to create SiteInsts to probe alternate site types...
        Design des = new Design("top",  args[0]);

        if (args[0].contains("xc7"))
            xc7_flag = true;

        //Design des = new Design("top", "xczu2cg-sbva484-1-e");
        Device d = des.getDevice();
        // Known constids
        Scanner scanner = new Scanner(new File(args[1]));
        int known_id_count = 0;
        makeConstId("");
        ++known_id_count;
        while (scanner.hasNextLine()) {
            String nl = scanner.nextLine().trim();
            if (nl.length() < 3 || !nl.substring(0, 2).equals("X("))
                continue;
            makeConstId(nl.substring(2, nl.length() - 1));
            ++known_id_count;
        }

        TimingModel tmg = new TimingModel(des);
        tmg.build();

        // Unique tiletypes
        HashSet<TileTypeEnum> seenTileTypes = new HashSet<>();
        for (Tile t : d.getAllTiles()) {
            if (seenTileTypes.contains(t.getTileTypeEnum()))
                continue;
            seenTileTypes.add(t.getTileTypeEnum());
            tileTypeIndices.put(t.getTileTypeEnum(), tileTypes.size());

            NextpnrTileType ntt = new NextpnrTileType();
            ntt.index = tileTypes.size();
            ntt.importTile(d, des, tmg, t);
            tileTypes.add(ntt);
            System.out.println("Processed tile type " + t.getTileTypeEnum().name());
        }

        // Tile entries

        HashMap<String, String> siteToPin = new HashMap<>();
        for (PackagePin p : d.getActivePackage().getPackagePinMap().values())
            if (p != null && p.getSite() != null)
                siteToPin.put(p.getSite().getName(), p.getName());

        for (int y = 0; y < d.getRows(); y++) {
            for (int x = 0; x < d.getColumns(); x++) {
                Tile t = d.getTile(y, x);
                NextpnrTileInst nti = new NextpnrTileInst();
                nti.name = t.getName();
                nti.type = tileTypeIndices.get(t.getTileTypeEnum());
                nti.index = tileInsts.size();
                nti.sites = new ArrayList<>();

                HashMap<String, Integer> site_offset_x = new HashMap<>(), site_offset_y = new HashMap<>();

                for (Site s : t.getSites()) {
                    String type = s.getSiteTypeEnum().toString();
                    if (type.startsWith("IOB33"))
                        type = "IOB33";
                    site_offset_x.putIfAbsent(type, s.getInstanceX());
                    site_offset_y.putIfAbsent(type, s.getInstanceY());
                    if (site_offset_x.get(type) > s.getInstanceX())
                        site_offset_x.put(type, s.getInstanceX());
                    if (site_offset_y.get(type) > s.getInstanceY())
                        site_offset_y.put(type, s.getInstanceY());
                }

                for (Site s : t.getSites()) {
                    NextpnrSiteInst nsi = new NextpnrSiteInst();
                    nsi.name = s.getName();
                    if (siteToPin.containsKey(s.getName()))
                        nsi.packagePin = siteToPin.get(s.getName());
                    else
                        nsi.packagePin = "."; // fixme: empty strings in bba
                    nsi.site_x = s.getInstanceX();
                    nsi.site_y = s.getInstanceY();
                    String type = s.getSiteTypeEnum().toString();
                    if (type.startsWith("IOB33"))
                        type = "IOB33";
                    nsi.rel_x = s.getInstanceX() - site_offset_x.get(type);
                    nsi.rel_y = s.getInstanceY() - site_offset_y.get(type);
                    Tile intert = null;
                    try {
                        intert = s.getIntTile();
                    } catch(java.lang.ArrayIndexOutOfBoundsException e) {

                    }
                    if (intert != null) {
                        nsi.inter_x = intert.getColumn();
                        nsi.inter_y = intert.getRow();
                    } else {
                        nsi.inter_x = -1;
                        nsi.inter_y = -1;
                    }
                    nti.sites.add(nsi);
                }
                nti.tilewire_to_node = new Integer[t.getWireCount() + 4]; // +1 accounts for vcc/ground pseudo-wires
                Arrays.fill(nti.tilewire_to_node, -1);
                tileInsts.add(nti);
                tileToTileInst.put(t.getRow() * d.getColumns() + t.getColumn(), nti);
            }
        }

        FileWriter bbaf = new FileWriter(args[2], false);
        PrintWriter bba = new PrintWriter(bbaf);


        // Header
        bba.println("pre #include \"nextpnr.h\"");
        bba.println("pre NEXTPNR_NAMESPACE_BEGIN");
        bba.println("post NEXTPNR_NAMESPACE_END");
        bba.println("push chipdb_blob");
        bba.println("offset32");
        bba.println("ref chip_info chip_info");

        bba.println("label extra_constid_strs");
        for (int i = known_id_count; i < constIds.size(); i++)
            bba.printf("str |%s|\n", constIds.get(i));
        bba.println("align");
        // Constant IDs additional to constids.inc
        bba.println("label extra_constids");
        bba.printf("u32 %d\n", known_id_count);
        bba.printf("u32 %d\n", constIds.size() - known_id_count);
        bba.println("ref extra_constid_strs");

        // Tiletypes
        for (NextpnrTileType tt : tileTypes) {
            // List of wires on bels in tile
            for (NextpnrBel b : tt.bels) {
                bba.printf("label t%db%d_wires\n", tt.index, b.index);
                for (NextpnrBelWire bw : b.belports) {
                    bba.printf("u32 %d\n", bw.name); // port name
                    bba.printf("u32 %d\n", bw.port_type); // port type
                    bba.printf("u32 %d\n", bw.wire); // index of connected tile wire
                }
            }
            // List of uphill pips, downhill pips and bel ports on wires in tile
            for (NextpnrWire w : tt.wires) {
                bba.printf("label t%dw%d_uh\n", tt.index, w.index);
                for (int uh : w.pips_uh) {
                    bba.printf("u32 %d\n", uh); // index of uphill pip
                }
                bba.printf("label t%dw%d_dh\n", tt.index, w.index);
                for (int dh : w.pips_dh) {
                    bba.printf("u32 %d\n", dh); // index of downhill pip
                }
                bba.printf("label t%dw%d_bels\n", tt.index, w.index);
                for (NextpnrBelPin bp : w.belpins) {
                    bba.printf("u32 %d\n", bp.bel); // index of bel in tile
                    bba.printf("u32 %d\n", bp.port); // bel port constid
                }
            }
            // Bel data for tiletype
            bba.printf("label t%d_bels\n", tt.index);
            for (NextpnrBel b : tt.bels) {
                bba.printf("u32 %d\n", b.name); //name constid
                bba.printf("u32 %d\n", b.type); //type (compatible type for nextpnr) constid
                bba.printf("u32 %d\n", b.nativeType); //native type (original type in RapidWright) constid
                bba.printf("u32 %d\n", -1); //FIXME: timing instance ID
                bba.printf("u32 %d\n", b.belports.size()); //number of bel port wires
                bba.printf("ref t%db%d_wires\n", tt.index, b.index); //ref to list of bel wires
                bba.printf("u16 %d\n", b.z); // bel z position
                bba.printf("u16 %d\n", b.site); // bel site index in tile
                bba.printf("u16 %d\n", b.siteVariant); // bel site variant
                bba.printf("u16 %d\n", b.isRouting);
            }

            // Wire data for tiletype
            bba.printf("label t%d_wires\n", tt.index);
            for (NextpnrWire w : tt.wires) {
                bba.printf("u32 %d\n", w.name); //name constid
                bba.printf("u32 %d\n", w.pips_uh.size()); //number of uphill pips
                bba.printf("u32 %d\n", w.pips_dh.size()); //number of downhill pips
                bba.printf("u32 %d\n", 0); //FIXME: timing class
                bba.printf("ref t%dw%d_uh\n", tt.index, w.index); //ref to list of uphill pips
                bba.printf("ref t%dw%d_dh\n", tt.index, w.index); //ref to list of downhill pips
                bba.printf("u32 %d\n",  w.belpins.size()); // number of bel pins
                bba.printf("ref t%dw%d_bels\n", tt.index, w.index); //ref to list of bel pins

                bba.printf("u16 %d\n", w.is_site ? w.site : -1); //site index or -1 if not a site wire
                bba.printf("u16 0\n"); //padding
                bba.printf("u32 %d\n", w.intent); //wire intent constid
            }

            // Pip data for tiletype
            bba.printf("label t%d_pips\n", tt.index);
            for (NextpnrPip p : tt.pips) {
                bba.printf("u32 %d\n", p.from); //src tile wire index
                bba.printf("u32 %d\n", p.to); //dst tile wire index
                bba.printf("u32 %d\n", p.tmg_cls);
                bba.printf("u16 %d\n", 0); // not used
                bba.printf("u16 %d\n", p.type.ordinal()); // pip type/flags

                bba.printf("u32 %d\n", p.bel); //bel name constid for site pips
                bba.printf("u32 %d\n", p.extra_data); //extra data for pseudo-pips
                bba.printf("u16 %d\n", p.site); //site index in tile for site pips
                bba.printf("u16 %d\n", p.siteVariant); //site variant index for site pips
            }

        }
        bba.printf("label tiletype_data\n");
        for (NextpnrTileType tt : tileTypes) {
            bba.printf("u32 %d\n", tt.type); //tile type name constid
            bba.printf("u32 %d\n", tt.bels.size()); //number of bels
            bba.printf("ref t%d_bels\n", tt.index); //ref to list of bels
            bba.printf("u32 %d\n", tt.wires.size()); //number of wires
            bba.printf("ref t%d_wires\n", tt.index); //ref to list of wires
            bba.printf("u32 %d\n", tt.pips.size()); //number of pips
            bba.printf("ref t%d_pips\n", tt.index); //ref to list of pips
            bba.printf("u32 %d\n", -1); //FIXME: timing class
        }

        // Nodes
        HashSet<TileTypeEnum> intTileTypes = Utils.getIntTileTypes();
        HashSet<Long> seenNodes = new HashSet<>();
        int curr = 0, total = d.getAllTiles().size();
        ArrayList<Integer> nodeWireCount = new ArrayList<>(), nodeIntent = new ArrayList<>();

        for (int row = 0; row < d.getRows(); row++) {
            HashSet<Node> gndNodes = new HashSet<>(), vccNodes = new HashSet<>();
            for (int col = 0; col < d.getColumns(); col++) {
                Tile t = d.getTile(row, col);
                ++curr;
                System.out.println("Processing nodes in tile " + curr + "/" + total);
                for (PIP p : t.getPIPs()) {
                    Node[] nodes = {p.getStartNode(), p.getEndNode()};
                    // FIXME: best way to discover nodes in tile?
                    for (Node n : nodes) {
                        long flatIndex = (long)(n.getTile().getRow() * d.getColumns() + n.getTile().getColumn()) << 32 | n.getWire();
                        if (seenNodes.contains(flatIndex))
                            continue;
                        seenNodes.add(flatIndex);

                        String wn = n.getWireName();
                        //System.out.println(t.getName() + " " + n.getWireName());
                        if ((t.getTileTypeEnum() != TileTypeEnum.BRAM_INT_INTERFACE_L && t.getTileTypeEnum() != TileTypeEnum.BRAM_INT_INTERFACE_R
                             && t.getTileTypeEnum() != TileTypeEnum.RCLK_INT_L && t.getTileTypeEnum() != TileTypeEnum.RCLK_INT_R) &&
                                n.isTiedToGnd()) {
                            gndNodes.add(n);
                            continue;
                        }
                        if ((t.getTileTypeEnum() != TileTypeEnum.BRAM_INT_INTERFACE_L && t.getTileTypeEnum() != TileTypeEnum.BRAM_INT_INTERFACE_R) && n.isTiedToVcc()) {
                            vccNodes.add(n);
                            continue;
                        }
                        if (n.getAllWiresInNode().length > 1) {
                            //nn.tilewires = new NextpnrTileWireRef[n.getAllWiresInNode().length];
                            bba.printf("label n%d_tw\n", nodeWireCount.size());
                            // Add interconnect tiles first for better delay estimates in nextpnr
                            for (int j = 0; j < 2; j++) {
                                for (Wire w : n.getAllWiresInNode()) {
                                    if (intTileTypes.contains(w.getTile().getTileTypeEnum()) != (j == 0))
                                        continue;
                                    int tileIndex = w.getTile().getRow() * d.getColumns() + w.getTile().getColumn();
                                    //nn.tilewires[i] = new NextpnrTileWireRef(tileToTileInst.get(tileIndex).index, w.getWireIndex());

                                    bba.printf("u32 %d\n", tileToTileInst.get(tileIndex).index); //tile inst index
                                    bba.printf("u32 %d\n", w.getWireIndex());

                                    tileToTileInst.get(tileIndex).tilewire_to_node[w.getWireIndex()] = nodeWireCount.size();

                                }
                            }
                            Wire nw = new Wire(n.getTile(), n.getWire());
                            nodeIntent.add(makeConstId(nw.getIntentCode().toString()));
                            nodeWireCount.add(n.getAllWiresInNode().length);
                        }
                    }
                }
            }
            // Connect up row and column ground nodes
            for (int i = 0; i < 2; i++) {
                bba.printf("label n%d_tw\n", nodeWireCount.size());
                int wireCount = 0;
                for (Node n : (i == 1) ? vccNodes : gndNodes) {
                    for (Wire w : n.getAllWiresInNode()) {
                        int tileIndex = w.getTile().getRow() * d.getColumns() + w.getTile().getColumn();
                        bba.printf("u32 %d\n", tileToTileInst.get(tileIndex).index);
                        bba.printf("u32 %d\n", w.getWireIndex());
                        tileToTileInst.get(tileIndex).tilewire_to_node[w.getWireIndex()] = nodeWireCount.size();
                        wireCount++;
                    }
                }

                for (int col = 0; col < d.getColumns(); col++) {
                    Tile t = d.getTile(row, col);
                    int tileIndex = t.getRow() * d.getColumns() + t.getColumn();
                    bba.printf("u32 %d\n", tileToTileInst.get(tileIndex).index);
                    int wireIndex = (i == 1) ? tileTypes.get(tileToTileInst.get(tileIndex).type).row_vcc_wire_index : tileTypes.get(tileToTileInst.get(tileIndex).type).row_gnd_wire_index;
                    bba.printf("u32 %d\n", wireIndex);
                    tileToTileInst.get(tileIndex).tilewire_to_node[wireIndex] = nodeWireCount.size();
                    wireCount++;
                }

                nodeWireCount.add(wireCount);
                nodeIntent.add(makeConstId(i == 1 ? "PSEUDO_VCC" : "PSEUDO_GND"));
            }
        }
        // Create the global Vcc and Ground nodes
        for (int i = 0; i < 2; i++) {
            bba.printf("label n%d_tw\n", nodeWireCount.size());
            int wireCount = 0;
            for (int row = 0; row < d.getRows(); row++) {
                Tile t = d.getTile(row, 0);
                int tileIndex = t.getRow() * d.getColumns() + t.getColumn();
                bba.printf("u32 %d\n", tileToTileInst.get(tileIndex).index);
                int wireIndex = (i == 1) ? tileTypes.get(tileToTileInst.get(tileIndex).type).global_vcc_wire_index : tileTypes.get(tileToTileInst.get(tileIndex).type).global_gnd_wire_index;
                bba.printf("u32 %d\n", wireIndex);
                tileToTileInst.get(tileIndex).tilewire_to_node[wireIndex] = nodeWireCount.size();
                wireCount++;
            }

            nodeWireCount.add(wireCount);
            nodeIntent.add(makeConstId(i == 1 ? "PSEUDO_VCC" : "PSEUDO_GND"));
        }

        for (NextpnrTileInst ti : tileInsts) {
            // Tilewire -> node mappings
            bba.printf("label ti%d_wire_to_node\n", ti.index);
            for (int w2n : ti.tilewire_to_node)
                bba.printf("u32 %d\n", w2n);
            bba.printf("label ti%d_sites\n", ti.index);
            for (NextpnrSiteInst si : ti.sites) {
                bba.printf("str |%s|\n", si.name);
                bba.printf("str |%s|\n", si.packagePin);
                bba.printf("u32 %d\n", si.site_x); //X nominal coordinate
                bba.printf("u32 %d\n", si.site_y); //Y nominal coordinate
                bba.printf("u32 %d\n", si.rel_x); //X nominal coordinate inside tile
                bba.printf("u32 %d\n", si.rel_y); //Y nominal coordinate inside tile
                bba.printf("u32 %d\n", si.inter_x); //X intercon tile coordinate
                bba.printf("u32 %d\n", si.inter_y); //Y intercon coordinate
            }
        }
        bba.printf("label tile_insts\n");
        for (NextpnrTileInst ti : tileInsts) {
            bba.printf("str |%s|\n", ti.name); //tile name
            bba.printf("u32 %d\n", ti.type); //tile type index into tiletype_data
            bba.printf("u32 %d\n", ti.tilewire_to_node.length); //length of tilewire_to_node
            bba.printf("ref ti%d_wire_to_node\n", ti.index); //ref to tilewire_to_node
            bba.printf("u32 %d\n", ti.sites.size());
            bba.printf("ref ti%d_sites\n", ti.index); //ref to list of site names
        }

        bba.printf("label nodes\n");
        for (int i = 0; i < nodeWireCount.size(); i++) {
            bba.printf("u32 %d\n", nodeWireCount.get(i)); //number of tilewires in node
            bba.printf("u32 %d\n", nodeIntent.get(i)); //node intent constid
            bba.printf("ref n%d_tw\n", i); //ref to list of tilewires
        }
        // FIXME: Placeholder timing data
        bba.println("label tile_cell_timing");
        // Nothing here yet
        bba.printf("label wire_timing_classes\n");
        bba.printf("u32 %d\n", 1); // resistance
        bba.printf("u32 %d\n", 0); // capacitance
        bba.printf("label pip_timing_classes\n");
        for (int dly : pipDelays) {
            bba.printf("u16 %d\n", 1); // is buffered
            bba.printf("u16 %d\n", 0); // padding
            bba.printf("u32 %d\n", dly); // min delay
            bba.printf("u32 %d\n", dly); // max delay
            bba.printf("u32 %d\n", 1); // resistance
            bba.printf("u32 %d\n", 0); // capacitance
        }
        bba.println("label timing");
        bba.printf("u32 %d\n", 0); // number of tile types with cell timing info
        bba.printf("u32 %d\n", 1); // number of wire classes
        bba.printf("u32 %d\n", pipDelays.size()); // number of pip classes
        bba.printf("ref tile_cell_timing\n");
        bba.printf("ref wire_timing_classes\n");
        bba.printf("ref pip_timing_classes\n");
        // Chip info
        bba.println("label chip_info");
        bba.printf("str |%s|\n", d.getDeviceName()); //device name
        bba.printf("str |RapidWright|\n"); //generator
        bba.printf("u32 %d\n", 1); //version
        bba.printf("u32 %d\n", d.getColumns()); //width
        bba.printf("u32 %d\n", d.getRows()); //height
        bba.printf("u32 %d\n", tileInsts.size()); //number of tiles
        bba.printf("u32 %d\n", tileTypes.size()); //number of tiletypes
        bba.printf("u32 %d\n", nodeWireCount.size()); //number of nodes
        bba.println("ref tiletype_data"); // reference to tiletype data
        bba.println("ref tile_insts"); // reference to tile instances
        bba.println("ref nodes"); // reference to node data
        bba.println("ref extra_constids"); // reference to bel data
        bba.printf("u32 %d\n", 1); // number of speed grades
        bba.println("ref timing"); // reference to bel data
        bba.println("pop");
        bbaf.close();
    }
}
