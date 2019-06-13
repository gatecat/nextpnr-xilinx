/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  David Shah <david@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <boost/algorithm/string.hpp>
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"
#include "util.h"
NEXTPNR_NAMESPACE_BEGIN

inline NetInfo *port_or_nullptr(const CellInfo *cell, IdString name)
{
    auto found = cell->ports.find(name);
    if (found == cell->ports.end())
        return nullptr;
    return found->second.net;
}

bool Arch::isBelLocationValid(BelId bel) const
{
    IdString belTileType = getBelTileType(bel);
    if (belTileType == id_CLEL_L || belTileType == id_CLEL_R || belTileType == id_CLEM || belTileType == id_CLEM_R) {
        // Logic Tile
        if (!tileStatus[bel.tile].lts)
            return true;
        LogicTileStatus &lts = *(tileStatus[bel.tile].lts);
        bool is_slicem = (belTileType == id_CLEM) || (belTileType == id_CLEM_R);
        bool tile_is_memory = false;
        if (lts.cells[(7 << 4) | BEL_6LUT] != nullptr && lts.cells[(7 << 4) | BEL_6LUT]->lutInfo.is_memory)
            tile_is_memory = true;
        // Check eight-tiles (mostly LUT-related validity)
        for (int i = 0; i < 8; i++) {
            if (lts.eights[i].dirty) {
                lts.eights[i].dirty = false;
                lts.eights[i].valid = false;

                CellInfo *lut6 = lts.cells[(i << 4) | BEL_6LUT];
                CellInfo *lut5 = lts.cells[(i << 4) | BEL_5LUT];

                // Check 6LUT
                if (lut6 != nullptr) {
                    if (!is_slicem && (lut6->lutInfo.is_memory || lut6->lutInfo.is_srl))
                        return false; // Memory and SRLs only valid in SLICEMs
                    if (lut5 != nullptr) {
                        // Can't mix memory and non-memory
                        if (lut6->lutInfo.is_memory != lut5->lutInfo.is_memory ||
                            lut6->lutInfo.is_srl != lut5->lutInfo.is_srl)
                            return false;
                        // If all 6 inputs or 2 outputs are used, 5LUT can't also be present
                        if (lut6->lutInfo.input_count == 6 || lut6->lutInfo.output_count == 2)
                            return false;
                        // If more than 5 total inputs are used, need to check number of shared input
                        if ((lut6->lutInfo.input_count + lut5->lutInfo.input_count) > 5) {
                            int shared = 0, need_shared = (lut6->lutInfo.input_count + lut5->lutInfo.input_count - 5);
                            for (int j = 0; j < lut6->lutInfo.input_count; j++) {
                                for (int k = 0; k < lut5->lutInfo.input_count; k++) {
                                    if (lut6->lutInfo.input_sigs[j] == lut5->lutInfo.input_sigs[k])
                                        shared++;
                                    if (shared >= need_shared)
                                        break;
                                }
                            }
                            if (shared < need_shared)
                                return false;
                        }
                    }
                }
                if (lut5 != nullptr) {
                    if (!is_slicem && (lut5->lutInfo.is_memory || lut5->lutInfo.is_srl))
                        return false; // Memory and SRLs only valid in SLICEMs
                    // 5LUT can use at most 5 inputs and 1 output
                    if (lut5->lutInfo.input_count > 5 || lut5->lutInfo.output_count == 2)
                        return false;
                }

                // Check (over)usage of DI and X inputs
                NetInfo *i_net = nullptr, *x_net = nullptr;
                if (lut6 != nullptr) {
                    i_net = lut6->lutInfo.di1_net;
                    x_net = lut6->lutInfo.di2_net;
                }
                if (lut5 != nullptr) {
                    if (lut5->lutInfo.di1_net != nullptr) {
                        if (i_net == nullptr)
                            i_net = lut5->lutInfo.di1_net;
                        else if (i_net != lut5->lutInfo.di1_net)
                            return false;
                    }
                    // DI2 not available for 5LUT
                    if (lut5->lutInfo.di2_net != nullptr)
                        return false;
                }

                CellInfo *mux = nullptr;
                // Eights A, C, E, G: F7MUX uses X input
                if (i == 0 || i == 2 || i == 4 || i == 6)
                    mux = lts.cells[i << 4 | BEL_F7MUX];
                // Eights B, F: F8MUX uses X input
                if (i == 1 || i == 5)
                    mux = lts.cells[(i - 1) << 4 | BEL_F8MUX];
                // Eights D: F9MUX uses X input
                if (i == 3)
                    mux = lts.cells[BEL_F9MUX];

                if (mux != nullptr) {
                    if (x_net == nullptr)
                        x_net = mux->muxInfo.sel;
                    else if (x_net != mux->muxInfo.sel)
                        return false;
                }

                CellInfo *out_fmux = nullptr;
                // Eights B, D, F, H: F7MUX connects to F7F8 out
                if (i == 1 || i == 3 || i == 5 || i == 7)
                    out_fmux = lts.cells[(i - 1) << 4 | BEL_F7MUX];
                // Eights C, G: F8MUX connects to F7F8 out
                if (i == 2 || i == 6)
                    out_fmux = lts.cells[(i - 2) << 4 | BEL_F8MUX];
                // Eights E: F9MUX connects to F7F8 out
                if (i == 4)
                    out_fmux = lts.cells[BEL_F9MUX];

                CellInfo *carry8 = lts.cells[BEL_CARRY8];
                // CARRY8 might use X
                if (carry8 != nullptr && carry8->carryInfo.x_sigs[i] != nullptr) {
                    if (x_net == nullptr)
                        x_net = carry8->carryInfo.x_sigs[i];
                    else if (x_net != carry8->carryInfo.x_sigs[i])
                        return false;
                }

                // FF1 might use X, if it isn't driven directly
                CellInfo *ff1 = lts.cells[i << 4 | BEL_FF];
                if (ff1 != nullptr && ff1->ffInfo.d != nullptr && ff1->ffInfo.d->driver.cell != nullptr) {
                    auto &drv = ff1->ffInfo.d->driver;
                    if ((drv.cell == lut6 && drv.port != id_MC31) || drv.cell == lut5 || drv.cell == out_fmux) {
                        // Direct, OK
                        // FIXME: CARRY8 direct
                    } else {
                        // Indirect, must use X input
                        if (x_net == nullptr)
                            x_net = ff1->ffInfo.d;
                        else if (x_net != ff1->ffInfo.d)
                            return false;
                    }
                }

                // FF2 might use I, if it isn't driven directly
                CellInfo *ff2 = lts.cells[i << 4 | BEL_FF2];
                if (ff2 != nullptr && ff2->ffInfo.d != nullptr && ff2->ffInfo.d->driver.cell != nullptr) {
                    auto &drv = ff2->ffInfo.d->driver;
                    if ((drv.cell == lut6 && drv.port != id_MC31) || drv.cell == lut5 || drv.cell == out_fmux) {
                        // Direct, OK
                        // FIXME: CARRY8 direct
                    } else {
                        // Indirect, must use X input
                        if (i_net == nullptr)
                            i_net = ff2->ffInfo.d;
                        else if (i_net != ff2->ffInfo.d)
                            return false;
                    }
                }

                if ((i == 3) || (i == 5) || (i == 6))
                    if (tile_is_memory && x_net != nullptr)
                        return false; // collision with top address bits

                bool mux_output_used = false;
                NetInfo *out5 = nullptr;
                if (lut6 != nullptr && lut6->lutInfo.output_count == 2)
                    out5 = lut6->lutInfo.output_sigs[1];
                else if (lut5 != nullptr && !lut5->lutInfo.only_drives_carry)
                    out5 = lut5->lutInfo.output_sigs[0];
                if (out5 != nullptr && (out5->users.size() > 1 || ((ff1 == nullptr || out5 != ff1->ffInfo.d) &&
                                                                   (ff2 == nullptr || out5 != ff2->ffInfo.d)))) {
                    mux_output_used = true;
                }

                if (carry8 != nullptr && carry8->carryInfo.out_sigs[i] != nullptr) {
                    // FIXME: direct connections to FF
                    if (mux_output_used)
                        return false;
                    mux_output_used = true;
                }
                if (out_fmux != nullptr) {
                    NetInfo *f7f8 = out_fmux->muxInfo.out;
                    if (f7f8 != nullptr && (f7f8->users.size() > 1 || ((ff1 == nullptr || f7f8 != ff1->ffInfo.d) &&
                                                                       (ff2 == nullptr || f7f8 != ff2->ffInfo.d)))) {
                        if (mux_output_used)
                            return false;
                        mux_output_used = true;
                    }
                }

                lts.eights[i].valid = true;
            } else if (!lts.eights[i].valid) {
                return false;
            }
        }
        // Check half-tiles
        for (int i = 0; i < 2; i++) {
            if (lts.halfs[i].dirty) {
                lts.halfs[i].valid = false;
                bool found_ff[2] = {false, false};
                NetInfo *clk = nullptr, *sr = nullptr, *ce[2] = {nullptr};
                bool clkinv = false, srinv = false, islatch = false;
                for (int z = 4 * i; z < 4 * (i + 1); z++) {
                    for (int k = 0; k < 2; k++) {
                        CellInfo *ff = lts.cells[z << 4 | (BEL_FF + k)];
                        if (ff == nullptr)
                            continue;
                        if (found_ff[0] || found_ff[1]) {
                            if (ff->ffInfo.clk != clk)
                                return false;
                            if (ff->ffInfo.sr != sr)
                                return false;
                            if (ff->ffInfo.is_clkinv != clkinv)
                                return false;
                            if (ff->ffInfo.is_srinv != srinv)
                                return false;
                            if (ff->ffInfo.is_latch != islatch)
                                return false;
                        } else {
                            clk = ff->ffInfo.clk;
                            sr = ff->ffInfo.sr;
                            clkinv = ff->ffInfo.is_clkinv;
                            srinv = ff->ffInfo.is_srinv;
                            islatch = ff->ffInfo.is_latch;
                        }
                        if (found_ff[k]) {
                            if (ff->ffInfo.ce != ce[k])
                                return false;
                        } else {
                            ce[k] = ff->ffInfo.ce;
                        }
                        found_ff[k] = true;
                    }
                }
                lts.halfs[i].valid = true;
            } else if (!lts.halfs[i].valid) {
                return false;
            }
        }
    } else {
        for (auto bel : getBelsByTile(bel.tile % chip_info->width, bel.tile / chip_info->width))
            if (getBoundBelCell(bel) != nullptr && usp_bel_hard_unavail(bel))
                return false;
    }
    return true;
}

bool Arch::isValidBelForCell(CellInfo *cell, BelId bel) const
{
    if (usp_bel_hard_unavail(bel))
        return false;
    return true;
}

void Arch::fixupPlacement()
{
    log_info("Running post-placement legalisation...\n");
    for (auto &ts : tileStatus) {
        if (ts.lts == nullptr)
            continue;
        auto &lt = *(ts.lts);
        for (int z = 0; z < 8; z++) {
            // Fixup LUT connectivity - applies whenever a LUT5 is used
            CellInfo *lut5 = lt.cells[z << 4 | BEL_5LUT];
            if (lut5 == nullptr)
                continue;
            std::unordered_map<IdString, std::vector<int>> lut5Inputs, lut6Inputs;
            for (int i = 0; i < lut5->lutInfo.input_count; i++)
                if (lut5->lutInfo.input_sigs[i])
                    lut5Inputs[lut5->lutInfo.input_sigs[i]->name].push_back(i);
            CellInfo *lut6 = lt.cells[z << 4 | BEL_6LUT];
            if (lut6) {
                for (int i = 0; i < lut6->lutInfo.input_count; i++)
                    if (lut6->lutInfo.input_sigs[i])
                        lut6Inputs[lut6->lutInfo.input_sigs[i]->name].push_back(i);
            }
            std::set<IdString> uniqueInputs;
            for (auto i5 : lut5Inputs)
                uniqueInputs.insert(i5.first);
            for (auto i6 : lut6Inputs)
                uniqueInputs.insert(i6.first);
            // Disconnect LUT inputs, and re-connect them to not overlap
            IdString ports[6] = {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6};
            for (auto p : ports) {
                disconnect_port(getCtx(), lut5, p);
                lut5->attrs.erase(id("X_ORIG_PORT_" + p.str(this)));
                if (lut6) {
                    lut6->attrs.erase(id("X_ORIG_PORT_" + p.str(this)));
                    disconnect_port(getCtx(), lut6, p);
                }
            }
            int index = 0;
            for (auto i : uniqueInputs) {
                if (lut5Inputs.count(i)) {
                    if (!lut5->ports.count(ports[index])) {
                        lut5->ports[ports[index]].name = ports[index];
                        lut5->ports[ports[index]].type = PORT_IN;
                    }
                    connect_port(getCtx(), nets.at(i).get(), lut5, ports[index]);
                    lut5->attrs[id("X_ORIG_PORT_" + ports[index].str(this))] = "";
                    bool first = true;
                    for (auto inp : lut5Inputs[i]) {
                        lut5->attrs[id("X_ORIG_PORT_" + ports[index].str(this))].str +=
                                (first ? "I" : " I") + std::to_string(inp);
                        first = false;
                    }
                }
                if (lut6 && lut6Inputs.count(i)) {
                    if (!lut6->ports.count(ports[index])) {
                        lut6->ports[ports[index]].name = ports[index];
                        lut6->ports[ports[index]].type = PORT_IN;
                    }
                    connect_port(getCtx(), nets.at(i).get(), lut6, ports[index]);

                    lut6->attrs[id("X_ORIG_PORT_" + ports[index].str(this))] = "";
                    bool first = true;
                    for (auto inp : lut6Inputs[i]) {
                        lut6->attrs[id("X_ORIG_PORT_" + ports[index].str(this))].str +=
                                (first ? "I" : " I") + std::to_string(inp);
                        first = false;
                    }
                }
                ++index;
            }
            rename_port(getCtx(), lut5, id_O6, id_O5);
            lut5->attrs.erase(id("X_ORIG_PORT_O6"));
            lut5->attrs[id("X_ORIG_PORT_O5")] = "O";

            if (lut6) {
                if (!lut6->ports.count(id_A6)) {
                    lut6->ports[id_A6].name = id_A6;
                    lut6->ports[id_A6].type = PORT_IN;
                }
                connect_port(getCtx(), nets[id("$PACKER_VCC_NET")].get(), lut6, id_A6);
            }
        }
    }
    for (auto cell : sorted(cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id("PSS_ALTO_CORE")) {
            log_info("Tieing unused PSS inputs to constants...\n");
            for (IdString pname : getBelPins(ci->bel)) {
                if (ci->ports.count(pname) && ci->ports.at(pname).net != nullptr &&
                    ci->ports.at(pname).net->driver.cell != nullptr)
                    continue;
                if (getBelPinType(ci->bel, pname) == PORT_OUT)
                    continue;
                std::string name = pname.str(this);
                if (name.find("_PAD_") != std::string::npos)
                    continue;
                if (name.find("PSVERSION") != std::string::npos || name.find("PSSGTS") != std::string::npos ||
                    name == "PSSGPWRDWNB" || name == "PSSGHIGHB" || name == "PSSFSTCFGB" || name == "PSSCFGRESETB" ||
                    name == "PCFGPORB" || name.find("IDCODE") != std::string::npos ||
                    name.find("BSCAN") != std::string::npos)
                    continue;
                bool constval = false;
                if (name == "NIRQ1LPDRPU" || name == "NIRQ0LPDRPU" || name == "NFIQ1LPDRPU" || name == "NFIQ0LPDRPU")
                    constval = true;
                if (boost::ends_with(name, "SSIN") || name == "EMIOSDIO1WP" || name == "EMIOSDIO0WP" ||
                    boost::ends_with(name, "RSOP") || boost::ends_with(name, "REOP") ||
                    boost::ends_with(name, "GMIITXCLK") || name == "DPVIDEOINCLK" || name == "DPSAXISAUDIOCLK")
                    constval = true;
                ci->ports[pname].name = pname;
                ci->ports[pname].type = PORT_IN;
                if (ci->ports[pname].net != nullptr) {
                    disconnect_port(getCtx(), ci, pname);
                    ci->attrs.erase(id("X_ORIG_PORT_" + name));
                }
                connect_port(getCtx(), nets[constval ? id("$PACKER_VCC_NET") : id("$PACKER_GND_NET")].get(), ci, pname);
            }
        }
    }
}

void Arch::fixupRouting()
{
    log_info("Running post-routing legalisation...\n");
    /*
     * Convert LUT permutation into correct physical connections (i.e. effectively eliminating the permutation pips),
     * then specifying the permutation as a new physical-to-logical mapping using X_ORIG_PORT. This keeps RapidWright
     * and Vivado happy, preserving the original logical netlist
     */
    std::unordered_map<int, std::vector<int>> used_perm_pips; // tile -> [extra_data] for LUT perm pips

    for (auto net : sorted(nets)) {
        NetInfo *ni = net.second;
        for (auto &wire : ni->wires) {
            PipId pip = wire.second.pip;
            if (pip == PipId())
                continue;
            auto &pd = locInfo(pip).pip_data[pip.index];
            if (pd.flags != PIP_LUT_PERMUTATION)
                continue;
            used_perm_pips[pip.tile].push_back(pd.extra_data);
        }
    }

    for (size_t ti = 0; ti < tileStatus.size(); ti++) {
        if (!used_perm_pips.count(int(ti)))
            continue;
        auto &ts = tileStatus.at(ti);
        if (ts.lts == nullptr)
            continue;

        auto &lt = *(ts.lts);
        for (int z = 0; z < 8; z++) {
            CellInfo *lut5 = lt.cells[z << 4 | BEL_5LUT];
            CellInfo *lut6 = lt.cells[z << 4 | BEL_6LUT];
            if (lut5 == nullptr && lut6 == nullptr)
                continue;
            auto &pp = used_perm_pips.at(ti);
            // from -> to
            std::unordered_map<IdString, std::vector<IdString>> new_connections;
            IdString ports[6] = {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6};
            for (auto pip : pp) {
                if (((pip >> 8) & 0xF) != z)
                    continue;
                new_connections[ports[(pip >> 4) & 0xF]].push_back(ports[pip & 0xF]);
            }
            std::unordered_map<IdString, NetInfo *> orig_nets;
            std::unordered_map<IdString, std::string> orig_ports_l6, orig_ports_l5;
            for (int i = 0; i < 6; i++) {
                NetInfo *l6net = lut6 ? get_net_or_empty(lut6, ports[i]) : nullptr;
                NetInfo *l5net = lut5 ? get_net_or_empty(lut5, ports[i]) : nullptr;
                orig_nets[ports[i]] = (l6net ? l6net : l5net);
                if (lut6)
                    orig_ports_l6[ports[i]] = str_or_default(lut6->attrs, id("X_ORIG_PORT_" + ports[i].str(this)));
                if (lut5)
                    orig_ports_l5[ports[i]] = str_or_default(lut5->attrs, id("X_ORIG_PORT_" + ports[i].str(this)));
            }
            for (auto &nc : new_connections) {
                if (lut6)
                    disconnect_port(getCtx(), lut6, nc.first);
                if (lut5)
                    disconnect_port(getCtx(), lut5, nc.first);
                for (auto &dst : nc.second) {
                    if (lut6)
                        disconnect_port(getCtx(), lut6, dst);
                    if (lut5)
                        disconnect_port(getCtx(), lut5, dst);
                }
            }
            for (int i = 0; i < 6; i++) {
                if (lut6)
                    lut6->attrs.erase(id("X_ORIG_PORT_" + ports[i].str(this)));
                if (lut5)
                    lut5->attrs.erase(id("X_ORIG_PORT_" + ports[i].str(this)));
            }
            for (int i = 0; i < 6; i++) {
                auto p = ports[i];
                if (!new_connections.count(p) || new_connections.at(p).empty())
                    continue;
                if (lut6) {
                    if (!lut6->ports.count(p)) {
                        lut6->ports[p].name = p;
                        lut6->ports[p].type = PORT_IN;
                    }
                    connect_port(getCtx(), orig_nets[new_connections.at(p).front()], lut6, p);
                    lut6->attrs[id("X_ORIG_PORT_" + p.str(this))] = "";
                    auto &orig_attr = lut6->attrs[id("X_ORIG_PORT_" + p.str(this))].str;
                    bool first = true;
                    for (auto &nc : new_connections.at(p)) {
                        orig_attr += orig_ports_l6[nc] + (first ? "" : " ");
                        first = false;
                    }
                    if (orig_attr.empty())
                        lut6->attrs.erase(id("X_ORIG_PORT_" + p.str(this)));
                }
                if (lut5) {
                    if (!lut5->ports.count(p)) {
                        lut5->ports[p].name = p;
                        lut5->ports[p].type = PORT_IN;
                    }
                    connect_port(getCtx(), orig_nets[new_connections.at(p).front()], lut5, p);
                    lut5->attrs[id("X_ORIG_PORT_" + p.str(this))] = "";
                    auto &orig_attr = lut5->attrs[id("X_ORIG_PORT_" + p.str(this))].str;
                    bool first = true;
                    for (auto &nc : new_connections.at(p)) {
                        orig_attr += orig_ports_l5[nc] + (first ? "" : " ");
                        first = false;
                    }
                    if (orig_attr.empty())
                        lut5->attrs.erase(id("X_ORIG_PORT_" + p.str(this)));
                }
            }
        }
    }
}

NEXTPNR_NAMESPACE_END
