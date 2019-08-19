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

#include "pack.h"
#include <algorithm>
#include <boost/optional.hpp>
#include <iterator>
#include <queue>
#include <unordered_set>
#include "cells.h"
#include "chain_utils.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "pins.h"

NEXTPNR_NAMESPACE_BEGIN

// Process the contents of packed_cells and new_cells
void XilinxPacker::flush_cells()
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

void XilinxPacker::xform_cell(const std::unordered_map<IdString, XFormRule> &rules, CellInfo *ci)
{
    auto &rule = rules.at(ci->type);
    ci->attrs[ctx->id("X_ORIG_TYPE")] = ci->type.str(ctx);
    ci->type = rule.new_type;
    std::vector<IdString> orig_port_names;
    for (auto &port : ci->ports)
        orig_port_names.push_back(port.first);

    for (auto pname : orig_port_names) {
        if (rule.port_multixform.count(pname)) {
            auto old_port = ci->ports.at(pname);
            disconnect_port(ctx, ci, pname);
            ci->ports.erase(pname);
            for (auto new_name : rule.port_multixform.at(pname)) {
                ci->ports[new_name].name = new_name;
                ci->ports[new_name].type = old_port.type;
                connect_port(ctx, old_port.net, ci, new_name);
                ci->attrs[ctx->id("X_ORIG_PORT_" + new_name.str(ctx))] = pname.str(ctx);
            }
        } else {
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

void XilinxPacker::generic_xform(const std::unordered_map<IdString, XFormRule> &rules, bool print_summary)
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

std::unique_ptr<CellInfo> XilinxPacker::feed_through_lut(NetInfo *net, const std::vector<PortRef> &feed_users)
{
    std::unique_ptr<NetInfo> feedthru_net{new NetInfo};
    feedthru_net->name = ctx->id(net->name.str(ctx) + "$legal$" + std::to_string(++autoidx));
    std::unique_ptr<CellInfo> lut = create_lut(ctx, net->name.str(ctx) + "$LUT$" + std::to_string(++autoidx), {net},
                                               feedthru_net.get(), Property(2));

    for (auto &usr : feed_users) {
        disconnect_port(ctx, usr.cell, usr.port);
        connect_port(ctx, feedthru_net.get(), usr.cell, usr.port);
    }

    IdString netname = feedthru_net->name;
    ctx->nets[netname] = std::move(feedthru_net);
    return lut;
}

IdString XilinxPacker::int_name(IdString base, const std::string &postfix, bool is_hierarchy)
{
    return ctx->id(base.str(ctx) + (is_hierarchy ? "$subcell$" : "$intcell$") + postfix);
}

NetInfo *XilinxPacker::create_internal_net(IdString base, const std::string &postfix, bool is_hierarchy)
{
    std::unique_ptr<NetInfo> net{new NetInfo};
    IdString name = ctx->id(base.str(ctx) + (is_hierarchy ? "$subnet$" : "$intnet$") + postfix);
    net->name = name;
    NPNR_ASSERT(!ctx->nets.count(name));
    ctx->nets[name] = std::move(net);
    return ctx->nets.at(name).get();
}

void XilinxPacker::pack_luts()
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

void XilinxPacker::pack_ffs()
{
    log_info("Packing flipflops..\n");

    std::unordered_map<IdString, XFormRule> ff_rules;
    ff_rules[ctx->id("FDCE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("FDCE")].port_xform[ctx->id("C")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("FDCE")].port_xform[ctx->id("CLR")] = id_SR;
    // ff_rules[ctx->id("FDCE")].param_xform[ctx->id("IS_CLR_INVERTED")] = ctx->id("IS_SR_INVERTED");

    ff_rules[ctx->id("FDPE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("FDPE")].port_xform[ctx->id("C")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("FDPE")].port_xform[ctx->id("PRE")] = id_SR;
    // ff_rules[ctx->id("FDPE")].param_xform[ctx->id("IS_PRE_INVERTED")] = ctx->id("IS_SR_INVERTED");

    ff_rules[ctx->id("FDRE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("FDRE")].port_xform[ctx->id("C")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("FDRE")].port_xform[ctx->id("R")] = id_SR;
    // ff_rules[ctx->id("FDRE")].param_xform[ctx->id("IS_R_INVERTED")] = ctx->id("IS_SR_INVERTED");

    ff_rules[ctx->id("FDSE")].new_type = id_SLICE_FFX;
    ff_rules[ctx->id("FDSE")].port_xform[ctx->id("C")] = ctx->xc7 ? id_CK : id_CLK;
    ff_rules[ctx->id("FDSE")].port_xform[ctx->id("S")] = id_SR;
    // ff_rules[ctx->id("FDSE")].param_xform[ctx->id("IS_S_INVERTED")] = ctx->id("IS_SR_INVERTED");

    generic_xform(ff_rules, true);
}

void XilinxPacker::pack_lutffs()
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

void XilinxPacker::pack_constants()
{
    log_info("Packing constants..\n");
    if (tied_pins.empty())
        get_tied_pins(ctx, tied_pins);
    if (invertible_pins.empty())
        get_invertible_pins(ctx, invertible_pins);
    if (!ctx->cells.count(ctx->id("$PACKER_GND_DRV"))) {
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

        ctx->cells[gnd_cell->name] = std::move(gnd_cell);
        ctx->nets[gnd_net->name] = std::move(gnd_net);
        ctx->cells[vcc_cell->name] = std::move(vcc_cell);
        ctx->nets[vcc_net->name] = std::move(vcc_net);
    }
    NetInfo *gnd = ctx->nets[ctx->id("$PACKER_GND_NET")].get(), *vcc = ctx->nets[ctx->id("$PACKER_VCC_NET")].get();

    std::vector<IdString> dead_nets;

    std::vector<std::tuple<CellInfo *, IdString, bool>> const_ports;

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (!tied_pins.count(ci->type))
            continue;
        auto &tp = tied_pins.at(ci->type);
        for (auto port : tp) {
            if (cell.second->ports.count(port.first) && cell.second->ports.at(port.first).net != nullptr &&
                cell.second->ports.at(port.first).net->driver.cell != nullptr)
                continue;
            const_ports.emplace_back(ci, port.first, port.second);
        }
    }

    for (auto net : sorted(ctx->nets)) {
        NetInfo *ni = net.second;
        if (ni->driver.cell != nullptr && ni->driver.cell->type == ctx->id("GND")) {
            IdString drv_cell = ni->driver.cell->name;
            for (auto &usr : ni->users) {
                const_ports.emplace_back(usr.cell, usr.port, false);
                usr.cell->ports.at(usr.port).net = nullptr;
            }
            dead_nets.push_back(net.first);
            ctx->cells.erase(drv_cell);
        } else if (ni->driver.cell != nullptr && ni->driver.cell->type == ctx->id("VCC")) {
            IdString drv_cell = ni->driver.cell->name;
            for (auto &usr : ni->users) {
                const_ports.emplace_back(usr.cell, usr.port, true);
                usr.cell->ports.at(usr.port).net = nullptr;
            }
            dead_nets.push_back(net.first);
            ctx->cells.erase(drv_cell);
        }
    }

    for (auto port : const_ports) {
        CellInfo *ci;
        IdString pname;
        bool cval;
        std::tie(ci, pname, cval) = port;

        if (!ci->ports.count(pname)) {
            ci->ports[pname].name = pname;
            ci->ports[pname].type = PORT_IN;
        }
        if (ci->ports.at(pname).net != nullptr) {
            // Case where a port with a default tie value is previously connected to an undriven net
            NPNR_ASSERT(ci->ports.at(pname).net->driver.cell == nullptr);
            disconnect_port(ctx, ci, pname);
        }

        if (!cval && invertible_pins.count(ci->type) && invertible_pins.at(ci->type).count(pname)) {
            // Invertible pins connected to zero are optimised to a connection to Vcc (which is easier to route)
            // and an inversion
            ci->params[ctx->id("IS_" + pname.str(ctx) + "_INVERTED")] = Property(1);
            cval = true;
        }

        connect_port(ctx, cval ? vcc : gnd, ci, pname);
    }

    for (auto dn : dead_nets) {
        ctx->nets.erase(dn);
    }
}

void XilinxPacker::rename_net(IdString old, IdString newname)
{
    std::unique_ptr<NetInfo> ni;
    std::swap(ni, ctx->nets[old]);
    ctx->nets.erase(old);
    ni->name = newname;
    ctx->nets[newname] = std::move(ni);
}

void USPacker::pack_bram()
{
    log_info("Packing BRAM..\n");

    // Rules for normal TDP BRAM
    std::unordered_map<IdString, XFormRule> bram_rules;
    bram_rules[ctx->id("RAMB18E2")].new_type = id_RAMB18E2_RAMB18E2;
    bram_rules[ctx->id("RAMB18E2")].port_multixform[ctx->id(std::string("WEA[0]"))] = {ctx->id("WEA0"),
                                                                                       ctx->id("WEA1")};
    bram_rules[ctx->id("RAMB18E2")].port_multixform[ctx->id(std::string("WEA[1]"))] = {ctx->id("WEA2"),
                                                                                       ctx->id("WEA3")};
    bram_rules[ctx->id("RAMB36E2")].new_type = id_RAMB36E2_RAMB36E2;

    // Some ports have upper/lower bel pins in 36-bit mode
    std::vector<std::pair<IdString, std::vector<std::string>>> ul_pins;
    get_bram36_ul_pins(ctx, ul_pins);
    for (auto &ul : ul_pins) {
        for (auto &bp : ul.second)
            bram_rules[ctx->id("RAMB36E2")].port_multixform[ul.first].push_back(ctx->id(bp));
    }
    bram_rules[ctx->id("RAMB36E2")].port_multixform[ctx->id("ECCPIPECE")] = {ctx->id("ECCPIPECEL")};

    // Special rules for SDP rules, relating to WE connectivity
    std::unordered_map<IdString, XFormRule> sdp_bram_rules = bram_rules;
    for (int i = 0; i < 2; i++) {
        // Connects to two WEBWE bel pins
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWE" + std::to_string(i * 2)));
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWE" + std::to_string(i * 2 + 1)));
        // Not used in SDP mode
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEA[" + std::to_string(i) + "]"))] = {};
    }
    for (int i = 0; i < 2; i++) {
        // Connects to two WEA bel pins
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 2) + "]"))]
                .push_back(ctx->id("WEA" + std::to_string(i * 2)));
        sdp_bram_rules[ctx->id("RAMB18E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 2) + "]"))]
                .push_back(ctx->id("WEA" + std::to_string(i * 2 + 1)));
    }

    for (int i = 0; i < 4; i++) {
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .clear();
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 4) + "]"))]
                .clear();
        // Connects to two WEBWE bel pins
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWEL" + std::to_string(i)));
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))]
                .push_back(ctx->id("WEBWEU" + std::to_string(i)));
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 4) + "]"))]
                .push_back(ctx->id("WEAL" + std::to_string(i)));
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i + 4) + "]"))]
                .push_back(ctx->id("WEAU" + std::to_string(i)));
        // Not used in SDP mode
        sdp_bram_rules[ctx->id("RAMB36E2")]
                .port_multixform[ctx->id(std::string("WEA[" + std::to_string(i) + "]"))] = {};
    }

    // 72-bit BRAMs: drop upper bits of WEB in TDP mode
    for (int i = 4; i < 8; i++)
        bram_rules[ctx->id("RAMB36E2")].port_multixform[ctx->id(std::string("WEBWE[" + std::to_string(i) + "]"))] = {};

    // Process SDP BRAM first
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if ((ci->type == ctx->id("RAMB18E2") &&
             int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_B")), 0) == 36) ||
            (ci->type == ctx->id("RAMB36E2") &&
             int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_B")), 0) == 72))
            xform_cell(sdp_bram_rules, ci);
    }

    // Rewrite byte enables according to data width
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("RAMB18E2") || ci->type == ctx->id("RAMB36E2")) {
            for (char port : {'A', 'B'}) {
                int write_width = int_or_default(ci->params, ctx->id(std::string("WRITE_WIDTH_") + port), 18);
                int we_width;
                if (ci->type == ctx->id("RAMB36E2"))
                    we_width = 4;
                else
                    we_width = (port == 'B') ? 4 : 2;
                if (write_width >= (9 * we_width))
                    continue;
                int used_we_width = std::max(write_width / 9, 1);
                for (int i = used_we_width; i < we_width; i++) {
                    NetInfo *low_we = get_net_or_empty(ci, ctx->id(std::string(port == 'B' ? "WEBWE[" : "WEA[") +
                                                                   std::to_string(i % used_we_width) + "]"));
                    IdString curr_we = ctx->id(std::string(port == 'B' ? "WEBWE[" : "WEA[") + std::to_string(i) + "]");
                    if (!ci->ports.count(curr_we)) {
                        ci->ports[curr_we].type = PORT_IN;
                        ci->ports[curr_we].name = curr_we;
                    }
                    disconnect_port(ctx, ci, curr_we);
                    connect_port(ctx, low_we, ci, curr_we);
                }
            }
        }
    }

    generic_xform(bram_rules, false);

    // These pins have no logical mapping, so must be tied after transformation
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id_RAMB18E2_RAMB18E2) {
            for (int i = 2; i < 4; i++) {
                IdString port = ctx->id("WEA" + std::to_string(i));
                if (!ci->ports.count(port)) {
                    ci->ports[port].name = port;
                    ci->ports[port].type = PORT_IN;
                    connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, port);
                }
            }
        }
    }
}

bool Arch::pack()
{
    if (xc7) {
        XC7Packer packer;
        packer.ctx = getCtx();
        packer.pack_constants();
        packer.pack_io();
        // packer.prepare_iologic();
        packer.prepare_clocking();
        packer.pack_constants();
        // packer.pack_iologic();
        // packer.pack_idelayctrl();
        packer.pack_clocking();
        // packer.pack_carries();
        packer.pack_luts();
        packer.pack_dram();
        // packer.pack_bram();
        packer.pack_ffs();
        packer.pack_lutffs();
    } else {
        USPacker packer;
        packer.ctx = getCtx();
        packer.pack_constants();
        packer.pack_io();
        packer.prepare_iologic();
        packer.prepare_clocking();
        packer.pack_constants();
        packer.pack_iologic();
        packer.pack_idelayctrl();
        packer.pack_clocking();
        packer.pack_carries();
        packer.pack_luts();
        packer.pack_dram();
        packer.pack_bram();
        packer.pack_ffs();
        packer.pack_lutffs();
    }

    assignArchInfo();
    attrs[id("step")] = std::string("pack");
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
        cell->ffInfo.clk = get_net_or_empty(cell, xc7 ? id_CK : id_CLK);
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