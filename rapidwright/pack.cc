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

#include <algorithm>
#include <boost/optional.hpp>
#include <iterator>
#include <queue>
#include <unordered_set>
#include "chain_utils.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

struct USPacker
{
    Context *ctx;

    // Generic cell transformation
    // Given cell name map and port map
    // If port name is not found in port map; it will be copied as-is but stripping []
    struct XFormRule
    {
        IdString new_type;
        std::unordered_map<IdString, IdString> port_xform;
        std::unordered_map<IdString, IdString> param_xform;
        std::vector<std::pair<IdString, std::string>> set_attrs;
        std::vector<std::pair<IdString, Property>> set_params;
    };

    std::unordered_set<IdString> packed_cells;
    std::vector<std::unique_ptr<CellInfo>> new_cells;

    // Process the contents of packed_cells and new_cells
    void flush_cells()
    {
        for (auto pcell : packed_cells) {
            for (auto &port : ctx->cells[pcell]->ports) {
                disconnect_port(ctx, ctx->cells[pcell].get(), port.first);
            }
            ctx->cells.erase(pcell);
        }
        for (auto &ncell : new_cells) {
            NPNR_ASSERT(!ctx->cells.count(ncell->name));
            ctx->cells[ncell->name] = std::move(ncell);
        }
        packed_cells.clear();
        new_cells.clear();
    }

    void xform_cell(const std::unordered_map<IdString, XFormRule> &rules, CellInfo *ci)
    {
        auto &rule = rules.at(ci->type);
        ci->attrs[ctx->id("X_ORIG_TYPE")] = ci->type.c_str(ctx);
        ci->type = rule.new_type;
        std::vector<IdString> orig_port_names;
        for (auto &port : ci->ports)
            orig_port_names.push_back(port.first);

        for (auto pname : orig_port_names) {
            IdString new_name;
            if (rule.port_xform.count(pname)) {
                new_name = rule.port_xform.at(pname);
            } else {
                std::string stripped_name;
                for (auto c : pname.str(ctx))
                    if (c != '[' && c != ']')
                        stripped_name += c;
                new_name = ctx->id(stripped_name);
            }
            if (new_name != pname) {
                rename_port(ctx, ci, pname, new_name);
            }
            ci->attrs[ctx->id("X_ORIG_PORT_" + new_name.str(ctx))] = pname.str(ctx);
        }

        std::vector<IdString> xform_params;
        for (auto &param : ci->params)
            if (rule.param_xform.count(param.first))
                xform_params.push_back(param.first);
        for (auto param : xform_params)
            ci->params[rule.param_xform.at(param)] = ci->params[param];

        for (auto &attr : rule.set_attrs)
            ci->attrs[attr.first] = attr.second;

        for (auto &param : rule.set_params)
            ci->params[param.first] = param.second;
    }

    void generic_xform(const std::unordered_map<IdString, XFormRule> &rules, bool print_summary = false)
    {
        std::map<std::string, int> cell_count;
        std::map<std::string, int> new_types;
        for (auto cell : sorted(ctx->cells)) {
            CellInfo *ci = cell.second;
            if (rules.count(ci->type)) {
                cell_count[ci->type.str(ctx)]++;
                xform_cell(rules, ci);
                new_types[ci->type.str(ctx)]++;
            }
        }
        if (print_summary) {
            for (auto &nt : new_types) {
                log_info("    Created %d %s cells from:\n", nt.second, nt.first.c_str());
                for (auto &cc : cell_count) {
                    if (rules.at(ctx->id(cc.first)).new_type != ctx->id(nt.first))
                        continue;
                    log_info("        %6dx %s\n", cc.second, cc.first.c_str());
                }
            }
        }
    }

    std::unique_ptr<CellInfo> create_cell(IdString type, IdString name)
    {
        std::unique_ptr<CellInfo> cell{new CellInfo};
        cell->type = type;
        cell->name = name;
        auto add_port = [&](const std::string &name, PortType dir) {
            IdString id = ctx->id(name);
            cell->ports[id].name = id;
            cell->ports[id].type = dir;
        };
        if (type == id_SLICE_LUTX) {
            for (int i = 1; i <= 6; i++)
                add_port("A" + std::to_string(i), PORT_IN);
            for (int i = 1; i <= 9; i++)
                add_port("WA" + std::to_string(i), PORT_IN);
            add_port("DI1", PORT_IN);
            add_port("DI2", PORT_IN);
            add_port("CLK", PORT_IN);
            add_port("WE", PORT_IN);
            add_port("SIN", PORT_IN);
            add_port("O5", PORT_OUT);
            add_port("O6", PORT_OUT);
            add_port("MC31", PORT_OUT);
        } else if (type == id_SLICE_FFX) {
            add_port("D", PORT_IN);
            add_port("SR", PORT_IN);
            add_port("CE", PORT_IN);
            add_port("CLK", PORT_IN);
            add_port("Q", PORT_OUT);
        } else if (type == ctx->id("RAMD64E")) {
            for (int i = 0; i < 6; i++)
                add_port("RADR" + std::to_string(i), PORT_IN);
            for (int i = 0; i < 8; i++)
                add_port("WADR" + std::to_string(i), PORT_IN);
            add_port("CLK", PORT_IN);
            add_port("I", PORT_IN);
            add_port("WE", PORT_IN);
            add_port("O", PORT_OUT);
        } else if (type == ctx->id("MUXF7") || type == ctx->id("MUXF8") || type == ctx->id("MUXF9")) {
            add_port("I0", PORT_IN);
            add_port("I1", PORT_IN);
            add_port("S", PORT_IN);
            add_port("O", PORT_OUT);
        } else if (type == ctx->id("CARRY8")) {
            add_port("CI", PORT_IN);
            add_port("CI_TOP", PORT_IN);

            for (int i = 0; i < 8; i++) {
                add_port("DI[" + std::to_string(i) + "]", PORT_IN);
                add_port("S[" + std::to_string(i) + "]", PORT_IN);
                add_port("CO[" + std::to_string(i) + "]", PORT_OUT);
                add_port("O[" + std::to_string(i) + "]", PORT_OUT);
            }
        } else if (type == ctx->id("MUXCY")) {
            add_port("CI", PORT_IN);
            add_port("DI", PORT_IN);
            add_port("S", PORT_IN);
            add_port("O", PORT_OUT);
        } else if (type == ctx->id("XORCY")) {
            add_port("CI", PORT_IN);
            add_port("LI", PORT_IN);
            add_port("O", PORT_OUT);
        }
        return cell;
    }

    std::unique_ptr<CellInfo> create_lut(const std::string &name, const std::vector<NetInfo *> &inputs, NetInfo *output,
                                         const Property &init)
    {
        std::unique_ptr<CellInfo> cell{new CellInfo};
        cell->type = ctx->id("LUT" + std::to_string(inputs.size()));
        cell->name = ctx->id(name);
        for (size_t i = 0; i < inputs.size(); i++) {
            IdString ip = ctx->id("I" + std::to_string(i));
            cell->ports[ip].name = ip;
            cell->ports[ip].type = PORT_IN;
            connect_port(ctx, inputs.at(i), cell.get(), ip);
        }
        cell->ports[ctx->id("O")].name = ctx->id("O");
        cell->ports[ctx->id("O")].type = PORT_OUT;
        connect_port(ctx, output, cell.get(), ctx->id("O"));
        cell->params[ctx->id("INIT")] = init;
        return cell;
    }

    int autoidx = 0;

    std::unique_ptr<CellInfo> feed_through_lut(NetInfo *net, const std::vector<PortRef> &feed_users)
    {
        std::unique_ptr<NetInfo> feedthru_net{new NetInfo};
        feedthru_net->name = ctx->id(net->name.str(ctx) + "$legal$" + std::to_string(++autoidx));
        std::unique_ptr<CellInfo> lut = create_lut(net->name.str(ctx) + "$LUT$" + std::to_string(++autoidx), {net},
                                                   feedthru_net.get(), Property(2));

        for (auto &usr : feed_users) {
            disconnect_port(ctx, usr.cell, usr.port);
            connect_port(ctx, feedthru_net.get(), usr.cell, usr.port);
        }

        IdString netname = feedthru_net->name;
        ctx->nets[netname] = std::move(feedthru_net);
        return lut;
    }

    void pack_luts()
    {
        log_info("Packing LUTs..\n");

        std::unordered_map<IdString, XFormRule> lut_rules;
        for (int k = 1; k <= 6; k++) {
            IdString lut = ctx->id("LUT" + std::to_string(k));
            lut_rules[lut].new_type = id_SLICE_LUTX;
            for (int i = 0; i < k; i++)
                lut_rules[lut].port_xform[ctx->id("I" + std::to_string(i))] = ctx->id("A" + std::to_string(i + 1));
            lut_rules[lut].port_xform[ctx->id("O")] = ctx->id("O6");
        }
        lut_rules[ctx->id("LUT6_2")] = lut_rules[ctx->id("LUT6")];
        generic_xform(lut_rules, true);
    }

    void pack_ffs()
    {
        log_info("Packing flipflops..\n");

        std::unordered_map<IdString, XFormRule> ff_rules;
        ff_rules[ctx->id("FDCE")].new_type = id_SLICE_FFX;
        ff_rules[ctx->id("FDCE")].port_xform[ctx->id("C")] = id_CLK;
        ff_rules[ctx->id("FDCE")].port_xform[ctx->id("CLR")] = id_SR;
        // ff_rules[ctx->id("FDCE")].param_xform[ctx->id("IS_CLR_INVERTED")] = ctx->id("IS_SR_INVERTED");

        ff_rules[ctx->id("FDPE")].new_type = id_SLICE_FFX;
        ff_rules[ctx->id("FDPE")].port_xform[ctx->id("C")] = id_CLK;
        ff_rules[ctx->id("FDPE")].port_xform[ctx->id("PRE")] = id_SR;
        // ff_rules[ctx->id("FDPE")].param_xform[ctx->id("IS_PRE_INVERTED")] = ctx->id("IS_SR_INVERTED");

        ff_rules[ctx->id("FDRE")].new_type = id_SLICE_FFX;
        ff_rules[ctx->id("FDRE")].port_xform[ctx->id("C")] = id_CLK;
        ff_rules[ctx->id("FDRE")].port_xform[ctx->id("R")] = id_SR;
        // ff_rules[ctx->id("FDRE")].param_xform[ctx->id("IS_R_INVERTED")] = ctx->id("IS_SR_INVERTED");

        ff_rules[ctx->id("FDSE")].new_type = id_SLICE_FFX;
        ff_rules[ctx->id("FDSE")].port_xform[ctx->id("C")] = id_CLK;
        ff_rules[ctx->id("FDSE")].port_xform[ctx->id("S")] = id_SR;
        // ff_rules[ctx->id("FDSE")].param_xform[ctx->id("IS_S_INVERTED")] = ctx->id("IS_SR_INVERTED");

        generic_xform(ff_rules, true);
    }

    void pack_lutffs()
    {
        int pairs = 0;
        for (auto cell : sorted(ctx->cells)) {
            CellInfo *ci = cell.second;
            if (ci->constr_parent != nullptr || !ci->constr_children.empty())
                continue;
            if (ci->type != id_SLICE_FFX)
                continue;
            NetInfo *d = get_net_or_empty(ci, id_D);
            if (d->driver.cell == nullptr || d->driver.cell->type != id_SLICE_LUTX || d->driver.port != id_O6)
                continue;
            CellInfo *lut = d->driver.cell;
            if (lut->constr_parent != nullptr || !lut->constr_children.empty())
                continue;
            lut->constr_children.push_back(ci);
            ci->constr_parent = lut;
            ci->constr_x = 0;
            ci->constr_y = 0;
            ci->constr_z = (BEL_FF - BEL_6LUT);
            ++pairs;
        }
        log_info("Constrained %d LUTFF pairs.\n", pairs);
    }

    // Distributed RAM control set
    struct DRAMControlSet
    {
        std::vector<NetInfo *> wa;
        NetInfo *wclk, *we;
        bool wclk_inv;

        bool operator==(const DRAMControlSet &other) const
        {
            return wa == other.wa && wclk == other.wclk && we == other.we && wclk_inv == other.wclk_inv;
        }
        bool operator!=(const DRAMControlSet &other) const
        {
            return wa != other.wa && wclk != other.wclk && we != other.we && wclk_inv != other.wclk_inv;
        }
    };

    struct DRAMControlSetHash
    {
        size_t operator()(const DRAMControlSet &dcs) const
        {
            std::size_t seed = 0;
            boost::hash_combine(seed, std::hash<size_t>()(dcs.wa.size()));
            for (auto abit : dcs.wa)
                boost::hash_combine(seed, std::hash<IdString>()(abit == nullptr ? IdString() : abit->name));
            boost::hash_combine(seed, std::hash<IdString>()(dcs.wclk == nullptr ? IdString() : dcs.wclk->name));
            boost::hash_combine(seed, std::hash<IdString>()(dcs.we == nullptr ? IdString() : dcs.we->name));
            boost::hash_combine(seed, std::hash<bool>()(dcs.wclk_inv));
            return seed;
        }
    };

    struct DRAMType
    {
        int abits;
        int dbits;
        int rports;
    };

    std::unordered_map<IdString, XFormRule> dram_rules;

    CellInfo *create_dram_lut(const std::string &name, CellInfo *base, const DRAMControlSet &ctrlset,
                              std::vector<NetInfo *> address, NetInfo *di, NetInfo *dout, int z)
    {
        std::unique_ptr<CellInfo> dram_lut = create_cell(ctx->id("RAMD64E"), ctx->id(name));
        for (int i = 0; i < int(address.size()); i++)
            connect_port(ctx, address[i], dram_lut.get(), ctx->id("RADR" + std::to_string(i)));
        connect_port(ctx, di, dram_lut.get(), ctx->id("I"));
        connect_port(ctx, dout, dram_lut.get(), ctx->id("O"));
        connect_port(ctx, ctrlset.wclk, dram_lut.get(), ctx->id("CLK"));
        connect_port(ctx, ctrlset.we, dram_lut.get(), ctx->id("WE"));
        for (int i = 0; i < int(ctrlset.wa.size()); i++)
            connect_port(ctx, ctrlset.wa[i], dram_lut.get(), ctx->id("WADR" + std::to_string(i)));
        dram_lut->params[ctx->id("IS_WCLK_INVERTED")] = ctrlset.wclk_inv ? "1" : "0";

        xform_cell(dram_rules, dram_lut.get());

        dram_lut->constr_abs_z = true;
        dram_lut->constr_z = (z << 4) | BEL_6LUT;
        if (base != nullptr) {
            dram_lut->constr_parent = base;
            dram_lut->constr_x = 0;
            dram_lut->constr_y = 0;
            base->constr_children.push_back(dram_lut.get());
        }
        // log_info("%s: z = %d (%d) -> %s\n", name.c_str(), z, dram_lut->constr_z, dram_lut->constr_parent ?
        // dram_lut->constr_parent->name.c_str(ctx) : "*");
        CellInfo *dl = dram_lut.get();
        new_cells.push_back(std::move(dram_lut));
        return dl;
    }

    void pack_dram()
    {

        log_info("Packing DRAM..\n");

        std::unordered_map<DRAMControlSet, std::vector<CellInfo *>, DRAMControlSetHash> dram_groups;
        std::unordered_map<IdString, DRAMType> dram_types;

        dram_types[ctx->id("RAM32X1S")] = {5, 1, 0};
        dram_types[ctx->id("RAM32X1D")] = {5, 1, 1};
        dram_types[ctx->id("RAM64X1S")] = {6, 1, 0};
        dram_types[ctx->id("RAM64X1D")] = {6, 1, 1};
        dram_types[ctx->id("RAM128X1S")] = {7, 1, 0};
        dram_types[ctx->id("RAM128X1D")] = {7, 1, 1};
        dram_types[ctx->id("RAM256X1S")] = {8, 1, 0};
        dram_types[ctx->id("RAM256X1D")] = {8, 1, 1};
        dram_types[ctx->id("RAM512X1S")] = {9, 1, 0};
        dram_types[ctx->id("RAM512X1D")] = {9, 1, 1};

        dram_types[ctx->id("RAM32X1M")] = {5, 2, 3};
        dram_types[ctx->id("RAM32X1M16")] = {5, 2, 7};

        // Transform from RAMD64E UNISIM to SLICE_LUTX bel
        dram_rules[ctx->id("RAMD64E")].new_type = id_SLICE_LUTX;
        dram_rules[ctx->id("RAMD64E")].param_xform[ctx->id("IS_CLK_INVERTED")] = ctx->id("IS_WCLK_INVERTED");
        dram_rules[ctx->id("RAMD64E")].set_attrs.emplace_back(ctx->id("X_LUT_AS_DRAM"), "1");
        for (int i = 0; i < 6; i++)
            dram_rules[ctx->id("RAMD64E")].port_xform[ctx->id("RADR" + std::to_string(i))] =
                    ctx->id("A" + std::to_string(i + 1));
        for (int i = 0; i < 8; i++)
            dram_rules[ctx->id("RAMD64E")].port_xform[ctx->id("WADR" + std::to_string(i))] =
                    ctx->id("WA" + std::to_string(i + 1));
        dram_rules[ctx->id("RAMD64E")].port_xform[ctx->id("I")] = id_DI1;
        dram_rules[ctx->id("RAMD64E")].port_xform[ctx->id("O")] = id_O6;

        // Optimise DRAM with tied-low inputs, to more efficiently routeable tied-high inputs
        int inverted_ports = 0;
        for (auto cell : sorted(ctx->cells)) {
            CellInfo *ci = cell.second;
            auto dt_iter = dram_types.find(ci->type);
            if (dt_iter == dram_types.end())
                continue;
            auto &dt = dt_iter->second;
            for (int i = 0; i < std::min(dt.abits, 6); i++) {
                IdString aport = ctx->id(dt.abits <= 6 ? ("A" + std::to_string(i)) : ("A[" + std::to_string(i) + "]"));
                if (!ci->ports.count(aport))
                    continue;
                NetInfo *anet = get_net_or_empty(ci, aport);
                if (anet == nullptr || anet->name != ctx->id("$PACKER_GND_NET"))
                    continue;
                IdString raport;
                if (dt.rports >= 1) {
                    NPNR_ASSERT(dt.rports == 1); // FIXME
                    raport =
                            ctx->id(dt.abits <= 6 ? ("DPRA" + std::to_string(i)) : ("DPRA[" + std::to_string(i) + "]"));
                    NetInfo *ranet = get_net_or_empty(ci, raport);
                    if (ranet == nullptr || ranet->name != ctx->id("$PACKER_GND_NET"))
                        continue;
                }
                disconnect_port(ctx, ci, aport);
                if (raport != IdString())
                    disconnect_port(ctx, ci, raport);
                connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, aport);
                if (raport != IdString())
                    connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, raport);
                ++inverted_ports;
                if (ci->params.count(ctx->id("INIT"))) {
                    Property &init = ci->params[ctx->id("INIT")];
                    if (init.isString()) {
                        // Binary init value
                        for (int j = 0; j < int(init.str.size()); j++) {
                            if (j & (1 << i))
                                init.str[j] = init.str[j & ~(1 << i)];
                        }
                    } else {
                        uint32_t int_init = init.num;
                        for (int j = 0; j < 32; j++) {
                            if (int_init & (1 << j)) {
                                if ((j & (1 << i)) && (int_init & (1 << (j & ~(1 << i))))) {
                                    int_init |= 1 << j;
                                }
                            }
                        }
                        init.num = int_init;
                    }
                }
            }
        }
        log_info("   Transformed %d tied-low DRAM address inputs to be tied-high\n", inverted_ports);

        for (auto cell : sorted(ctx->cells)) {
            CellInfo *ci = cell.second;
            auto dt_iter = dram_types.find(ci->type);
            if (dt_iter == dram_types.end())
                continue;
            auto &dt = dt_iter->second;
            DRAMControlSet dcs;
            for (int i = 0; i < dt.abits; i++)
                dcs.wa.push_back(get_net_or_empty(
                        ci, ctx->id(dt.abits <= 6 ? ("A" + std::to_string(i)) : ("A[" + std::to_string(i) + "]"))));
            dcs.wclk = get_net_or_empty(ci, ctx->id("WCLK"));
            dcs.we = get_net_or_empty(ci, ctx->id("WE"));
            dcs.wclk_inv = bool_or_default(ci->params, ctx->id("IS_WCLK_INVERTED"));
            dram_groups[dcs].push_back(ci);
        }

        for (auto &group : dram_groups) {
            int z = 7;
            CellInfo *base = nullptr;
            auto &cs = group.first;
            for (auto cell : group.second) {
                NPNR_ASSERT(cell->type == ctx->id("RAM64X1D")); // FIXME

                int z_size = 0;
                if (get_net_or_empty(cell, ctx->id("SPO")) != nullptr)
                    z_size++;
                if (get_net_or_empty(cell, ctx->id("DPO")) != nullptr)
                    z_size++;

                if (z == 7 || (z - z_size + 1) < 0) {
                    z = 7;
                    // Topmost cell is the write address input
                    std::vector<NetInfo *> address(cs.wa.begin(), cs.wa.begin() + std::min<size_t>(cs.wa.size(), 6));
                    base = create_dram_lut(cell->name.str(ctx) + "/ADDR", nullptr, cs, address, nullptr, nullptr, z);
                    z--;
                }

                NetInfo *dpo = get_net_or_empty(cell, ctx->id("DPO"));
                NetInfo *spo = get_net_or_empty(cell, ctx->id("SPO"));
                disconnect_port(ctx, cell, ctx->id("DPO"));
                disconnect_port(ctx, cell, ctx->id("SPO"));

                NetInfo *di = get_net_or_empty(cell, ctx->id("D"));
                if (spo != nullptr) {
                    if (z == 6) {
                        // Can fold DPO into address buffer
                        connect_port(ctx, spo, base, ctx->id("O"));
                        connect_port(ctx, di, base, ctx->id("I"));
                        if (cell->params.count(ctx->id("INIT")))
                            base->params[ctx->id("INIT")] = cell->params[ctx->id("INIT")];
                    } else {
                        std::vector<NetInfo *> address(cs.wa.begin(),
                                                       cs.wa.begin() + std::min<size_t>(cs.wa.size(), 6));
                        CellInfo *dpr = create_dram_lut(cell->name.str(ctx) + "/SP", base, cs, address, di, spo, z);
                        if (cell->params.count(ctx->id("INIT")))
                            dpr->params[ctx->id("INIT")] = cell->params[ctx->id("INIT")];
                        z--;
                    }
                }

                if (dpo != nullptr) {
                    std::vector<NetInfo *> address;
                    for (int i = 0; i < 6; i++)
                        address.push_back(get_net_or_empty(cell, ctx->id("DPRA" + std::to_string(i))));
                    CellInfo *dpr = create_dram_lut(cell->name.str(ctx) + "/DP", base, cs, address, di, dpo, z);
                    if (cell->params.count(ctx->id("INIT")))
                        dpr->params[ctx->id("INIT")] = cell->params[ctx->id("INIT")];
                    z--;
                }

                packed_cells.insert(cell->name);
            }
        }

        flush_cells();
    }

    void pack_constants()
    {
        log_info("Packing constants..\n");

        std::unique_ptr<CellInfo> gnd_cell{new CellInfo};
        gnd_cell->name = ctx->id("$PACKER_GND_DRV");
        gnd_cell->type = id_PSEUDO_GND;
        gnd_cell->ports[id_Y].name = id_Y;
        gnd_cell->ports[id_Y].type = PORT_OUT;
        std::unique_ptr<NetInfo> gnd_net = std::unique_ptr<NetInfo>(new NetInfo);
        gnd_net->name = ctx->id("$PACKER_GND_NET");
        gnd_net->driver.cell = gnd_cell.get();
        gnd_net->driver.port = id_Y;
        gnd_cell->ports.at(id_Y).net = gnd_net.get();

        std::unique_ptr<CellInfo> vcc_cell{new CellInfo};
        vcc_cell->name = ctx->id("$PACKER_VCC_DRV");
        vcc_cell->type = id_PSEUDO_VCC;
        vcc_cell->ports[id_Y].name = id_Y;
        vcc_cell->ports[id_Y].type = PORT_OUT;
        std::unique_ptr<NetInfo> vcc_net = std::unique_ptr<NetInfo>(new NetInfo);
        vcc_net->name = ctx->id("$PACKER_VCC_NET");
        vcc_net->driver.cell = vcc_cell.get();
        vcc_net->driver.port = id_Y;
        vcc_cell->ports.at(id_Y).net = vcc_net.get();

        std::vector<IdString> dead_nets;

        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            if (ni->driver.cell != nullptr && ni->driver.cell->type == ctx->id("GND")) {
                IdString drv_cell = ni->driver.cell->name;
                for (auto &usr : ni->users) {

                    // Tie FF CE and SR high and invert, instead of connecting to GND as this is cheaper
                    if ((usr.cell->type == ctx->id("FDRE") || usr.cell->type == ctx->id("FDCE") ||
                         usr.cell->type == ctx->id("FDPE") || usr.cell->type == ctx->id("FDSE")) &&
                        (usr.port == id_CE || usr.port == ctx->id("S") || usr.port == ctx->id("R") ||
                         usr.port == ctx->id("PRE") || usr.port == ctx->id("CLR"))) {
                        usr.cell->params[ctx->id("IS_" + usr.port.str(ctx) + "_INVERTED")] = "1";
                        usr.cell->ports.at(usr.port).net = vcc_net.get();
                        vcc_net->users.push_back(usr);
                    } else {
                        usr.cell->ports.at(usr.port).net = gnd_net.get();
                        gnd_net->users.push_back(usr);
                    }
                }
                dead_nets.push_back(net.first);
                ctx->cells.erase(drv_cell);
            } else if (ni->driver.cell != nullptr && ni->driver.cell->type == ctx->id("VCC")) {
                IdString drv_cell = ni->driver.cell->name;
                for (auto &usr : ni->users) {
                    usr.cell->ports.at(usr.port).net = vcc_net.get();
                    vcc_net->users.push_back(usr);
                }
                dead_nets.push_back(net.first);
                ctx->cells.erase(drv_cell);
            }
        }

        ctx->cells[gnd_cell->name] = std::move(gnd_cell);
        ctx->nets[gnd_net->name] = std::move(gnd_net);
        ctx->cells[vcc_cell->name] = std::move(vcc_cell);
        ctx->nets[vcc_net->name] = std::move(vcc_net);

        for (auto dn : dead_nets) {
            ctx->nets.erase(dn);
        }
    }

    struct CarryGroup
    {
        std::vector<CellInfo *> muxcys;
        std::vector<CellInfo *> xorcys;
    };

    bool has_illegal_fanout(NetInfo *carry)
    {
        // FIXME: sometimes we can feed out of the chain
        if (carry->users.size() > 2)
            return true;
        CellInfo *muxcy = nullptr, *xorcy = nullptr;
        for (auto &usr : carry->users) {
            if (usr.cell->type == ctx->id("MUXCY")) {
                if (muxcy != nullptr)
                    return true;
                else if (usr.port != ctx->id("CI"))
                    return true;
                else
                    muxcy = usr.cell;
            } else if (usr.cell->type == ctx->id("XORCY")) {
                if (xorcy != nullptr)
                    return true;
                else if (usr.port != ctx->id("CI"))
                    return true;
                else
                    xorcy = usr.cell;
            } else {
                return true;
            }
        }
        if (muxcy && xorcy) {
            NetInfo *muxcy_s = get_net_or_empty(muxcy, ctx->id("S"));
            NetInfo *xorcy_li = get_net_or_empty(xorcy, ctx->id("LI"));
            if (muxcy_s != xorcy_li)
                return true;
        }
        return false;
    }

    void pack_carries()
    {
        log_info("Packing carries..\n");
        std::vector<CellInfo *> root_muxcys;
        // Find MUXCYs
        for (auto cell : sorted(ctx->cells)) {
            CellInfo *ci = cell.second;
            if (ci->type != ctx->id("MUXCY"))
                continue;
            NetInfo *ci_net = get_net_or_empty(ci, ctx->id("CI"));
            if (ci_net == nullptr || ci_net->driver.cell == nullptr || ci_net->driver.cell->type != ctx->id("MUXCY") ||
                has_illegal_fanout(ci_net)) {
                root_muxcys.push_back(ci);
            }
        }

        // Create chains from root MUXCYs
        std::unordered_set<IdString> processed_muxcys;
        std::vector<CarryGroup> groups;
        int muxcy_count = 0, xorcy_count = 0;
        for (auto root : root_muxcys) {
            CarryGroup group;

            CellInfo *muxcy = root;
            NetInfo *mux_ci = nullptr;
            while (true) {

                group.muxcys.push_back(muxcy);
                ++muxcy_count;
                mux_ci = get_net_or_empty(muxcy, ctx->id("CI"));
                NetInfo *mux_s = get_net_or_empty(muxcy, ctx->id("S"));
                group.xorcys.push_back(nullptr);
                if (mux_s != nullptr) {
                    for (auto &user : mux_s->users) {
                        if (user.cell->type == ctx->id("XORCY") && user.port == ctx->id("LI")) {
                            CellInfo *xorcy = user.cell;
                            NetInfo *xor_ci = get_net_or_empty(xorcy, ctx->id("CI"));
                            if (xor_ci == mux_ci) {
                                group.xorcys.back() = xorcy;
                                ++xorcy_count;
                                break;
                            }
                        }
                    }
                }

                mux_ci = get_net_or_empty(muxcy, ctx->id("O"));
                if (mux_ci == nullptr)
                    break;
                if (has_illegal_fanout(mux_ci))
                    break;
                muxcy = nullptr;
                for (auto &user : mux_ci->users) {
                    if (user.cell->type == ctx->id("MUXCY")) {
                        muxcy = user.cell;
                        break;
                    }
                }
                if (muxcy == nullptr)
                    break;
            }
            if (mux_ci != nullptr) {
                if (mux_ci->users.size() == 1 && mux_ci->users.at(0).cell->type == ctx->id("XORCY") &&
                    mux_ci->users.at(0).port == ctx->id("CI")) {
                    // Trailing XORCY at end, can pack into chain.
                    CellInfo *xorcy = mux_ci->users.at(0).cell;
                    std::unique_ptr<CellInfo> dummy_muxcy =
                            create_cell(ctx->id("MUXCY"), ctx->id(xorcy->name.str(ctx) + "$legal_muxcy$"));
                    connect_port(ctx, mux_ci, dummy_muxcy.get(), ctx->id("CI"));
                    connect_port(ctx, get_net_or_empty(xorcy, ctx->id("LI")), dummy_muxcy.get(), ctx->id("S"));
                    group.muxcys.push_back(dummy_muxcy.get());
                    group.xorcys.push_back(xorcy);
                    new_cells.push_back(std::move(dummy_muxcy));
                } else if (mux_ci->users.size() > 0) {
                    // Users other than a MUXCY
                    // Feed out with a zero-driving LUT and a XORCY
                    // (creating a zero-driver using Vcc and an inverter for now...)
                    std::unique_ptr<CellInfo> zero_lut =
                            create_lut(mux_ci->name.str(ctx) + "$feed$zero",
                                       {ctx->nets[ctx->id("$PACKER_VCC_NET")].get()}, nullptr, Property(1));
                    std::unique_ptr<CellInfo> feed_xorcy =
                            create_cell(ctx->id("XORCY"), ctx->id(mux_ci->name.str(ctx) + "$feed$xor"));
                    std::unique_ptr<CellInfo> dummy_muxcy =
                            create_cell(ctx->id("MUXCY"), ctx->id(mux_ci->name.str(ctx) + "$feed$muxcy"));

                    CellInfo *last_muxcy = mux_ci->driver.cell;

                    disconnect_port(ctx, last_muxcy, ctx->id("O"));

                    connect_ports(ctx, zero_lut.get(), ctx->id("O"), feed_xorcy.get(), ctx->id("LI"));
                    connect_ports(ctx, zero_lut.get(), ctx->id("O"), dummy_muxcy.get(), ctx->id("S"));
                    connect_ports(ctx, last_muxcy, ctx->id("O"), feed_xorcy.get(), ctx->id("CI"));
                    connect_ports(ctx, last_muxcy, ctx->id("O"), dummy_muxcy.get(), ctx->id("CI"));

                    connect_port(ctx, mux_ci, feed_xorcy.get(), ctx->id("O"));

                    group.muxcys.push_back(dummy_muxcy.get());
                    group.xorcys.push_back(feed_xorcy.get());
                    new_cells.push_back(std::move(zero_lut));
                    new_cells.push_back(std::move(feed_xorcy));
                    new_cells.push_back(std::move(dummy_muxcy));
                }
            }

            groups.push_back(group);
        }
        flush_cells();

        log_info("   Grouped %d MUXCYs and %d XORCYs into %d chains.\n", muxcy_count, xorcy_count,
                 int(root_muxcys.size()));

        // N.B. LUT6 is not a valid type here, as CARRY requires dual outputs
        std::unordered_set<IdString> lut_types{ctx->id("LUT1"), ctx->id("LUT2"), ctx->id("LUT3"), ctx->id("LUT4"),
                                               ctx->id("LUT5")};

        std::unordered_set<IdString> folded_nets;

        for (auto &grp : groups) {
            std::vector<std::unique_ptr<CellInfo>> carry8s;
            for (int i = 0; i < int(grp.muxcys.size()); i++) {
                int z = i % 8;
                CellInfo *muxcy = grp.muxcys.at(i), *xorcy = grp.xorcys.at(i);
                if (z == 0)
                    carry8s.push_back(
                            create_cell(ctx->id("CARRY8"), ctx->id(muxcy->name.str(ctx) + "$PACKED_CARRY8$")));
                CellInfo *c8 = carry8s.back().get();
                CellInfo *root = carry8s.front().get();
                if (i == 0) {
                    // Constrain initial CARRY8, forcing it to the CARRY8 of a logic tile
                    c8->constr_abs_z = true;
                    c8->constr_z = BEL_CARRY8;
                } else if (z == 0) {
                    // Constrain relative to the root carry8
                    c8->constr_parent = root;
                    root->constr_children.push_back(c8);
                    c8->constr_x = 0;
                    c8->constr_y = -i / 8;
                    c8->constr_abs_z = true;
                    c8->constr_z = BEL_CARRY8;
                }
                // Fold CI->CO connections into the CARRY8, except for those external ones every 8 units
                if (z == 0) {
                    replace_port(muxcy, ctx->id("CI"), c8, ctx->id("CI"));
                } else {
                    NetInfo *muxcy_ci = get_net_or_empty(muxcy, ctx->id("CI"));
                    if (muxcy_ci)
                        folded_nets.insert(muxcy_ci->name);
                    disconnect_port(ctx, muxcy, ctx->id("CI"));
                }
                if (z == 7) {
                    replace_port(muxcy, ctx->id("O"), c8, ctx->id("CO[7]"));
                } else {
                    NetInfo *muxcy_o = get_net_or_empty(muxcy, ctx->id("O"));
                    if (muxcy_o)
                        folded_nets.insert(muxcy_o->name);
                    disconnect_port(ctx, muxcy, ctx->id("O"));
                }
                // Replace connections into the MUXCY with external CARRY8 ports
                replace_port(muxcy, ctx->id("S"), c8, ctx->id("S[" + std::to_string(z) + "]"));
                replace_port(muxcy, ctx->id("DI"), c8, ctx->id("DI[" + std::to_string(z) + "]"));
                packed_cells.insert(muxcy->name);
                // Fold MUXCY->XORCY into the CARRY8, if there is a XORCY
                if (xorcy) {
                    // Replace XORCY output with external CARRY8 output
                    replace_port(xorcy, ctx->id("O"), c8, ctx->id("O[" + std::to_string(z) + "]"));
                    // Disconnect internal XORCY connectivity
                    disconnect_port(ctx, xorcy, ctx->id("LI"));
                    disconnect_port(ctx, xorcy, ctx->id("DI"));
                    packed_cells.insert(xorcy->name);
                }
                // Check legality of LUTs driving CARRY8, making them legal if they aren't already
                NetInfo *c8_s = get_net_or_empty(c8, ctx->id("S[" + std::to_string(z) + "]"));
                NetInfo *c8_di = get_net_or_empty(c8, ctx->id("DI[" + std::to_string(z) + "]"));
                // Keep track of the total LUT input count; cannot exceed five or the LUTs cannot be packed together
                std::unordered_set<IdString> unique_lut_inputs;
                int s_inputs = 0, d_inputs = 0;
                // Check that S and DI are validy and unqiuely driven by LUTs
                // FIXME: in multiple fanout cases, cell duplication will probably be cheaper
                // than feed-throughs
                CellInfo *s_lut = nullptr, *di_lut = nullptr;
                if (c8_s) {
                    if (c8_s->users.size() == 1 && c8_s->driver.cell != nullptr &&
                        lut_types.count(c8_s->driver.cell->type)) {
                        s_lut = c8_s->driver.cell;
                        for (int j = 0; j < 5; j++) {
                            NetInfo *ix = get_net_or_empty(s_lut, ctx->id("I[" + std::to_string(i) + "]"));
                            if (ix) {
                                unique_lut_inputs.insert(ix->name);
                                s_inputs++;
                            }
                        }
                    }
                }
                if (c8_di) {
                    if (c8_di->users.size() == 1 && c8_di->driver.cell != nullptr &&
                        lut_types.count(c8_di->driver.cell->type)) {
                        di_lut = c8_di->driver.cell;
                        for (int j = 0; j < 5; j++) {
                            NetInfo *ix = get_net_or_empty(di_lut, ctx->id("I[" + std::to_string(i) + "]"));
                            if (ix) {
                                unique_lut_inputs.insert(ix->name);
                                d_inputs++;
                            }
                        }
                    }
                }
                int lut_inp_count = int(unique_lut_inputs.size());
                if (!s_lut)
                    ++lut_inp_count; // for feedthrough
                if (!di_lut)
                    ++lut_inp_count; // for feedthrough
                if (lut_inp_count > 5) {
                    // Must use feedthrough for at least one LUT
                    di_lut = nullptr;
                    if (s_inputs > 4)
                        s_lut = nullptr;
                }
                // If LUTs are nullptr, that means we need a feedthrough lut
                if (!s_lut && c8_s) {
                    PortRef pr;
                    pr.cell = c8;
                    pr.port = ctx->id("S[" + std::to_string(z) + "]");
                    auto s_feed = feed_through_lut(c8_s, {pr});
                    s_lut = s_feed.get();
                    new_cells.push_back(std::move(s_feed));
                }
                if (!di_lut && c8_di) {
                    PortRef pr;
                    pr.cell = c8;
                    pr.port = ctx->id("DI[" + std::to_string(z) + "]");
                    auto di_feed = feed_through_lut(c8_di, {pr});
                    di_lut = di_feed.get();
                    new_cells.push_back(std::move(di_feed));
                }
                // Constrain LUTs relative to root CARRY8
                if (s_lut) {
                    root->constr_children.push_back(s_lut);
                    s_lut->constr_parent = root;
                    s_lut->constr_x = 0;
                    s_lut->constr_y = -i / 8;
                    s_lut->constr_abs_z = true;
                    s_lut->constr_z = (z << 4 | BEL_6LUT);
                }
                if (di_lut) {
                    root->constr_children.push_back(di_lut);
                    di_lut->constr_parent = root;
                    di_lut->constr_x = 0;
                    di_lut->constr_y = -i / 8;
                    di_lut->constr_abs_z = true;
                    di_lut->constr_z = (z << 4 | BEL_5LUT);
                }
            }
            for (auto &c8 : carry8s)
                new_cells.push_back(std::move(c8));
        }
        flush_cells();

        for (auto net : folded_nets)
            ctx->nets.erase(net);

        // XORCYs and MUXCYs not part of any chain (and therefore not packed into a CARRY8) must now be blasted
        // to boring soft logic (LUT2 or LUT3 - these will become SLICE_LUTXs later in the flow.)
        int remaining_muxcy = 0, remaining_xorcy = 0;
        for (auto &cell : ctx->cells) {
            if (cell.second->type == ctx->id("MUXCY"))
                ++remaining_muxcy;
            else if (cell.second->type == ctx->id("XORCY"))
                ++remaining_xorcy;
        }
        std::unordered_map<IdString, XFormRule> softlogic_rules;
        softlogic_rules[ctx->id("MUXCY")].new_type = ctx->id("LUT3");
        softlogic_rules[ctx->id("MUXCY")].port_xform[ctx->id("DI")] = ctx->id("I0");
        softlogic_rules[ctx->id("MUXCY")].port_xform[ctx->id("CI")] = ctx->id("I1");
        softlogic_rules[ctx->id("MUXCY")].port_xform[ctx->id("S")] = ctx->id("I2");
        // DI 1010 1010
        // CI 1100 1100
        //  S 1111 0000
        //  O 1100 1010
        softlogic_rules[ctx->id("MUXCY")].set_params.emplace_back(ctx->id("INIT"), Property(0xCA));

        softlogic_rules[ctx->id("XORCY")].new_type = ctx->id("LUT2");
        softlogic_rules[ctx->id("XORCY")].port_xform[ctx->id("CI")] = ctx->id("I0");
        softlogic_rules[ctx->id("XORCY")].port_xform[ctx->id("LI")] = ctx->id("I1");
        // CI 1100
        // LI 1010
        //  O 0110
        softlogic_rules[ctx->id("XORCY")].set_params.emplace_back(ctx->id("INIT"), Property(0x6));

        generic_xform(softlogic_rules, false);
        log_info("   Blasted %d non-chain MUXCYs and %d non-chain XORCYs to soft logic\n", remaining_muxcy,
                 remaining_xorcy);

        // Finally, use generic_xform to remove the [] from bus ports; and set up the logical-physical mapping for
        // RapidWright
        std::unordered_map<IdString, XFormRule> c8_chained_rules;
        c8_chained_rules[ctx->id("CARRY8")].new_type = ctx->id("CARRY8");
        c8_chained_rules[ctx->id("CARRY8")].port_xform[ctx->id("CI")] = ctx->id("CIN");
        c8_chained_rules[ctx->id("CARRY8")].set_params.emplace_back(ctx->id("CARRY_TYPE"), Property("SINGLE_CY8"));
        std::unordered_map<IdString, XFormRule> c8_init_rules;
        c8_init_rules[ctx->id("CARRY8")].new_type = ctx->id("CARRY8");
        c8_init_rules[ctx->id("CARRY8")].port_xform[ctx->id("CI")] = ctx->id("AX");
        c8_init_rules[ctx->id("CARRY8")].set_params.emplace_back(ctx->id("CARRY_TYPE"), Property("SINGLE_CY8"));

        for (auto cell : sorted(ctx->cells)) {
            CellInfo *ci = cell.second;
            if (ci->type != ctx->id("CARRY8"))
                continue;
            if (ci->constr_parent == nullptr)
                xform_cell(c8_init_rules, ci);
            else
                xform_cell(c8_chained_rules, ci);
            ci->ports[ctx->id("EX")].name = ctx->id("EX");
            ci->ports[ctx->id("EX")].type = PORT_IN;
            connect_port(ctx, ctx->nets[ctx->id("$PACKER_GND_NET")].get(), ci, ctx->id("EX"));
        }
    }

    void pack_io()
    {
        log_info("Inserting IO buffers..\n");
        std::unordered_map<IdString, XFormRule> io_rules;
        io_rules[ctx->id("$nextpnr_ibuf")].new_type = ctx->id("IOB_IBUFCTRL");
        io_rules[ctx->id("$nextpnr_ibuf")].set_attrs.emplace_back(ctx->id("X_ORIG_TYPE"), "IBUFCTRL");

        io_rules[ctx->id("$nextpnr_obuf")].new_type = ctx->id("IOB_OUTBUF");
        io_rules[ctx->id("$nextpnr_obuf")].set_attrs.emplace_back(ctx->id("X_ORIG_TYPE"), "OBUF");

        io_rules[ctx->id("BUFGCTRL")].new_type = ctx->id("BUFGCTRL");
        io_rules[ctx->id("OBUF")].new_type = ctx->id("IOB_OUTBUF");
        io_rules[ctx->id("INBUF")].new_type = ctx->id("IOB_INBUF");
        io_rules[ctx->id("IBUFCTRL")].new_type = ctx->id("IOB_IBUFCTRL");

        io_rules[ctx->id("PS8")].new_type = ctx->id("PSS_ALTO_CORE");
        io_rules[ctx->id("BUFG_PS")].new_type = ctx->id("BUFCE_BUFG_PS");

        generic_xform(io_rules, true);
    }
};

bool Arch::pack()
{
    USPacker packer;
    packer.ctx = getCtx();
    packer.pack_io();
    packer.pack_constants();
    packer.pack_carries();
    packer.pack_luts();
    packer.pack_dram();
    packer.pack_ffs();
    packer.pack_lutffs();
    assignArchInfo();
    attrs[id("step")] = "pack";
    archInfoToAttributes();
    return true;
}

void Arch::assignCellInfo(CellInfo *cell)
{
    if (cell->type == id_SLICE_LUTX) {
        cell->lutInfo.input_count = 0;
        for (IdString a : {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6}) {
            NetInfo *pn = get_net_or_empty(cell, a);
            if (pn != nullptr)
                cell->lutInfo.input_sigs[cell->lutInfo.input_count++] = pn;
        }
        cell->lutInfo.output_count = 0;
        for (IdString o : {id_O6, id_O5}) {
            NetInfo *pn = get_net_or_empty(cell, o);
            if (pn != nullptr)
                cell->lutInfo.output_sigs[cell->lutInfo.output_count++] = pn;
        }
        cell->lutInfo.di1_net = get_net_or_empty(cell, id_DI1);
        cell->lutInfo.di2_net = get_net_or_empty(cell, id_DI2);
        cell->lutInfo.memory_group = 0; // fixme
        cell->lutInfo.is_srl = cell->attrs.count(id("X_LUT_AS_SRL"));
        cell->lutInfo.is_memory = cell->attrs.count(id("X_LUT_AS_DRAM"));
        cell->lutInfo.only_drives_carry = false;
        if (cell->constr_parent != nullptr && cell->lutInfo.output_sigs[0] != nullptr &&
            cell->lutInfo.output_sigs[0]->users.size() == 1 &&
            cell->lutInfo.output_sigs[0]->users.at(0).cell->type == id_CARRY8)
            cell->lutInfo.only_drives_carry = true;
    } else if (cell->type == id_SLICE_FFX) {
        cell->ffInfo.d = get_net_or_empty(cell, id_D);
        cell->ffInfo.clk = get_net_or_empty(cell, id_CLK);
        cell->ffInfo.ce = get_net_or_empty(cell, id_CE);
        cell->ffInfo.sr = get_net_or_empty(cell, id_SR);
        cell->ffInfo.is_clkinv = bool_or_default(cell->params, id("IS_CLK_INVERTED"), false);
        cell->ffInfo.is_srinv = bool_or_default(cell->params, id("IS_R_INVERTED"), false) ||
                                bool_or_default(cell->params, id("IS_S_INVERTED"), false) ||
                                bool_or_default(cell->params, id("IS_CLR_INVERTED"), false) ||
                                bool_or_default(cell->params, id("IS_PRE_INVERTED"), false);
        cell->ffInfo.is_latch = cell->attrs.count(id("X_FF_AS_LATCH"));
    } else if (cell->type == id_F7MUX || cell->type == id_F8MUX || cell->type == id_F9MUX) {
        cell->muxInfo.sel = get_net_or_empty(cell, id_S0);
        cell->muxInfo.out = get_net_or_empty(cell, id_OUT);
    } else if (cell->type == id_CARRY8) {
        for (int i = 0; i < 8; i++) {
            cell->carryInfo.out_sigs[i] = get_net_or_empty(cell, id("O" + std::to_string(i)));
            cell->carryInfo.cout_sigs[i] = get_net_or_empty(cell, id("CO" + std::to_string(i)));
            cell->carryInfo.x_sigs[i] = get_net_or_empty(cell, id(std::string(1, 'A' + i) + "X"));
        }
    }
}

void Arch::assignArchInfo()
{
    for (auto cell : sorted(cells)) {
        assignCellInfo(cell.second);
    }
}

NEXTPNR_NAMESPACE_END