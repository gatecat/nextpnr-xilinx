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
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <iterator>
#include <queue>
#include <unordered_set>
#include "cells.h"
#include "chain_utils.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "pack.h"
#include "pins.h"

NEXTPNR_NAMESPACE_BEGIN

CellInfo *XC7Packer::insert_ibuf(IdString name, IdString type, NetInfo *i, NetInfo *o)
{
    auto inbuf = create_cell(ctx, type, name);
    connect_port(ctx, i, inbuf.get(), ctx->id("I"));
    connect_port(ctx, o, inbuf.get(), ctx->id("O"));
    CellInfo *inbuf_ptr = inbuf.get();
    new_cells.push_back(std::move(inbuf));
    return inbuf_ptr;
}

CellInfo *XC7Packer::insert_diffibuf(IdString name, IdString type, const std::array<NetInfo *, 2> &i, NetInfo *o)
{
    auto inbuf = create_cell(ctx, type, name);
    connect_port(ctx, i[0], inbuf.get(), ctx->id("I"));
    connect_port(ctx, i[1], inbuf.get(), ctx->id("IB"));
    connect_port(ctx, o, inbuf.get(), ctx->id("O"));
    CellInfo *inbuf_ptr = inbuf.get();
    new_cells.push_back(std::move(inbuf));
    return inbuf_ptr;
}

void XC7Packer::decompose_iob(CellInfo *xil_iob, bool is_hr, const std::string &iostandard)
{
    bool is_se_ibuf = xil_iob->type == ctx->id("IBUF") || xil_iob->type == ctx->id("IBUF_IBUFDISABLE") ||
                      xil_iob->type == ctx->id("IBUF_INTERMDISABLE");
    bool is_se_iobuf = xil_iob->type == ctx->id("IOBUF") || xil_iob->type == ctx->id("IOBUF_DCIEN") ||
                       xil_iob->type == ctx->id("IOBUF_INTERMDISABLE");
    bool is_se_obuf = xil_iob->type == ctx->id("OBUF") || xil_iob->type == ctx->id("OBUFT");

    auto pad_site = [&](NetInfo *n) {
        for (auto user : n->users)
            if (user.cell->type == ctx->id("PAD"))
                return ctx->getBelSite(ctx->getBelByName(ctx->id(user.cell->attrs[ctx->id("BEL")].as_string())));
        NPNR_ASSERT_FALSE(("can't find PAD for net " + n->name.str(ctx)).c_str());
    };

    /*
     * IO primitives in Xilinx are complex "macros" that usually expand to more than one BEL
     * To avoid various nasty bugs (such as auto-transformation by Vivado of dedicated INV primitives to LUT1s), we
     * have to maintain this hierarchy so it can be re-built during DCP conversion in RapidWright
     */
    std::unordered_map<IdString, PortInfo> orig_ports = xil_iob->ports;
    std::vector<CellInfo *> subcells;

    if (is_se_ibuf || is_se_iobuf) {
        NetInfo *pad_net = get_net_or_empty(xil_iob, is_se_iobuf ? ctx->id("IO") : ctx->id("I"));
        NPNR_ASSERT(pad_net != nullptr);
        std::string site = pad_site(pad_net);
        if (!is_se_iobuf)
            disconnect_port(ctx, xil_iob, ctx->id("I"));

        NetInfo *top_out = get_net_or_empty(xil_iob, ctx->id("O"));
        disconnect_port(ctx, xil_iob, ctx->id("O"));

        IdString ibuf_type = ctx->id("IBUF");
        if (xil_iob->type == ctx->id("IBUF_IBUFDISABLE") || xil_iob->type == ctx->id("IOBUF_DCIEN"))
            ibuf_type = ctx->id("IBUF_IBUFDISABLE");
        if (xil_iob->type == ctx->id("IBUF_INTERMDISABLE") || xil_iob->type == ctx->id("IOBUF_INTERMDISABLE"))
            ibuf_type = ctx->id("IBUF_INTERMDISABLE");

        CellInfo *inbuf = insert_ibuf(int_name(xil_iob->name, "IBUF", is_se_iobuf), ibuf_type, pad_net, top_out);
        inbuf->attrs[ctx->id("BEL")] = site + "/IOB33/INBUF_EN";
        replace_port(xil_iob, ctx->id("IBUFDISABLE"), inbuf, ctx->id("IBUFDISABLE"));
        replace_port(xil_iob, ctx->id("INTERMDISABLE"), inbuf, ctx->id("INTERMDISABLE"));

        if (is_se_iobuf)
            subcells.push_back(inbuf);
    }

    if (is_se_obuf || is_se_iobuf) {
        NetInfo *pad_net = get_net_or_empty(xil_iob, is_se_iobuf ? ctx->id("IO") : ctx->id("O"));
        NPNR_ASSERT(pad_net != nullptr);
        std::string site = pad_site(pad_net);
        disconnect_port(ctx, xil_iob, is_se_iobuf ? ctx->id("IO") : ctx->id("O"));
        bool has_dci = xil_iob->type == ctx->id("IOBUF_DCIEN");
        CellInfo *obuf = insert_obuf(
                int_name(xil_iob->name, (is_se_iobuf || xil_iob->type == ctx->id("OBUFT")) ? "OBUFT" : "OBUF",
                         !is_se_obuf),
                is_se_iobuf ? (has_dci ? ctx->id("OBUFT_DCIEN") : ctx->id("OBUFT")) : xil_iob->type,
                get_net_or_empty(xil_iob, ctx->id("I")), pad_net, get_net_or_empty(xil_iob, ctx->id("T")));
        obuf->attrs[ctx->id("BEL")] = site + "/IOB33/OUTBUF";
        replace_port(xil_iob, ctx->id("DCITERMDISABLE"), obuf, ctx->id("DCITERMDISABLE"));
        if (is_se_iobuf)
            subcells.push_back(obuf);
    }

    if (!subcells.empty()) {
        for (auto sc : subcells) {
            sc->attrs[ctx->id("X_ORIG_MACRO_PRIM")] = xil_iob->type.str(ctx);
            for (auto &p : sc->ports) {
                std::string macro_ports;
                for (auto &orig : orig_ports) {
                    if ((orig.second.net != nullptr) && (orig.second.net == p.second.net)) {
                        macro_ports += orig.first.str(ctx);
                        macro_ports += ',';
                        macro_ports += (orig.second.type == PORT_INOUT) ? "inout"
                                                                        : (orig.second.type == PORT_OUT) ? "out" : "in";
                        macro_ports += ";";
                    }
                }
                if (!macro_ports.empty()) {
                    macro_ports.erase(macro_ports.size() - 1);
                    sc->attrs[ctx->id("X_MACRO_PORTS_" + p.first.str(ctx))] = macro_ports;
                }
            }
        }
    }
}

void XC7Packer::pack_io()
{

    log_info("Inserting IO buffers..\n");

    get_top_level_pins(ctx, toplevel_ports);
    // Insert PAD cells on top level IO, and IO buffers where one doesn't exist already
    std::vector<std::pair<CellInfo *, PortRef>> pad_and_buf;
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("$nextpnr_ibuf") || ci->type == ctx->id("$nextpnr_iobuf") ||
            ci->type == ctx->id("$nextpnr_obuf"))
            pad_and_buf.push_back(insert_pad_and_buf(ci));
    }
    flush_cells();
    std::unordered_set<BelId> used_io_bels;
    int unconstr_io_count = 0;
    for (auto &iob : pad_and_buf) {
        CellInfo *pad = iob.first;
        // Process location constraints
        if (pad->attrs.count(ctx->id("LOC"))) {
            std::string loc = pad->attrs.at(ctx->id("LOC")).as_string();
            std::string site = ctx->getPackagePinSite(loc);
            if (site.empty())
                log_error("Unable to constrain IO '%s', device does not have a pin named '%s'\n", pad->name.c_str(ctx),
                          loc.c_str());
            log_info("    Constraining '%s' to site '%s'\n", pad->name.c_str(ctx), site.c_str());
            pad->attrs[ctx->id("BEL")] = std::string(site + "/IOB33/PAD");
        }
        if (pad->attrs.count(ctx->id("BEL"))) {
            used_io_bels.insert(ctx->getBelByName(ctx->id(pad->attrs.at(ctx->id("BEL")).as_string())));
        } else {
            ++unconstr_io_count;
        }
    }
    std::queue<BelId> available_io_bels;
    IdString pad_id = ctx->id("IOB_PAD");
    for (auto bel : ctx->getBels()) {
        if (int(available_io_bels.size()) >= unconstr_io_count)
            break;
        if (ctx->getBelType(bel) != pad_id)
            continue;
        if (ctx->getBelPackagePin(bel) == ".")
            continue;
        if (used_io_bels.count(bel))
            continue;
        available_io_bels.push(bel);
    }
    // Constrain unconstrained IO
    for (auto &iob : pad_and_buf) {
        CellInfo *pad = iob.first;
        if (!pad->attrs.count(ctx->id("BEL"))) {
            pad->attrs[ctx->id("BEL")] = std::string(ctx->nameOfBel(available_io_bels.front()));
            available_io_bels.pop();
        }
    }
    // Decompose macro IO primitives to smaller primitives that map logically to the actual IO Bels
    for (auto &iob : pad_and_buf) {
        if (packed_cells.count(iob.second.cell->name))
            continue;
        decompose_iob(iob.second.cell, true, str_or_default(iob.first->attrs, ctx->id("IOSTANDARD"), ""));
        packed_cells.insert(iob.second.cell->name);
    }
    flush_cells();

    std::unordered_map<IdString, XFormRule> hrio_rules;
    hrio_rules[ctx->id("PAD")].new_type = ctx->id("PAD");
    hrio_rules[ctx->id("OBUF")].new_type = ctx->id("IOB33_OUTBUF");
    hrio_rules[ctx->id("OBUF")].port_xform[ctx->id("I")] = ctx->id("IN");
    hrio_rules[ctx->id("OBUF")].port_xform[ctx->id("O")] = ctx->id("OUT");
    hrio_rules[ctx->id("OBUF")].port_xform[ctx->id("T")] = ctx->id("TRI");
    hrio_rules[ctx->id("OBUFT")] = hrio_rules[ctx->id("OBUF")];

    hrio_rules[ctx->id("IBUF")].new_type = ctx->id("IOB33_INBUF_EN");
    hrio_rules[ctx->id("IBUF")].port_xform[ctx->id("I")] = ctx->id("PAD");
    hrio_rules[ctx->id("IBUF")].port_xform[ctx->id("O")] = ctx->id("OUT");
    hrio_rules[ctx->id("IBUF_INTERMDISABLE")] = hrio_rules[ctx->id("IBUF")];
    hrio_rules[ctx->id("IBUF_IBUFDISABLE")] = hrio_rules[ctx->id("IBUF")];
    hrio_rules[ctx->id("IBUFDS_INTERMDISABLE_INT")] = hrio_rules[ctx->id("IBUF")];
    hrio_rules[ctx->id("IBUFDS_INTERMDISABLE_INT")].port_xform[ctx->id("IB")] = ctx->id("DIFF_IN");

    generic_xform(hrio_rules, true);
}

std::string XC7Packer::get_ologic_site(const std::string &io_bel)
{
    BelId ibc_bel = ctx->getBelByName(ctx->id(io_bel.substr(0, io_bel.find('/')) + "/IOB33/OUTBUF"));
    std::queue<WireId> visit;
    visit.push(ctx->getBelPinWire(ibc_bel, ctx->id("IN")));

    while (!visit.empty()) {
        WireId cursor = visit.front();
        visit.pop();
        for (auto bp : ctx->getWireBelPins(cursor)) {
            std::string site = ctx->getBelSite(bp.bel);
            if (boost::starts_with(site, "OLOGIC"))
                return site;
        }
        for (auto pip : ctx->getPipsUphill(cursor))
            visit.push(ctx->getPipSrcWire(pip));
    }
    NPNR_ASSERT_FALSE("failed to find OLOGIC");
}

std::string XC7Packer::get_ilogic_site(const std::string &io_bel)
{
    BelId ibc_bel = ctx->getBelByName(ctx->id(io_bel.substr(0, io_bel.find('/')) + "/IOB33/INBUF_EN"));
    std::queue<WireId> visit;
    visit.push(ctx->getBelPinWire(ibc_bel, ctx->id("OUT")));

    while (!visit.empty()) {
        WireId cursor = visit.front();
        visit.pop();
        for (auto bp : ctx->getWireBelPins(cursor)) {
            std::string site = ctx->getBelSite(bp.bel);
            if (boost::starts_with(site, "ILOGIC"))
                return site;
        }
        for (auto pip : ctx->getPipsDownhill(cursor))
            visit.push(ctx->getPipDstWire(pip));
    }
    NPNR_ASSERT_FALSE("failed to find ILOGIC");
}

void XC7Packer::fold_inverter(CellInfo *cell, std::string port)
{
    IdString p = ctx->id(port);
    NetInfo *net = get_net_or_empty(cell, p);
    if (net == nullptr)
        return;
    CellInfo *drv = net->driver.cell;
    if (drv == nullptr || drv->type != ctx->id("LUT1") || int_or_default(drv->params, ctx->id("INIT"), 0) != 1)
        return;
    disconnect_port(ctx, cell, p);
    NetInfo *preinv = get_net_or_empty(drv, ctx->id("I0"));
    connect_port(ctx, preinv, cell, p);
    cell->params[ctx->id("IS_" + port + "_INVERTED")] = 1;
    if (net->users.empty())
        packed_cells.insert(drv->name);
}

void XC7Packer::pack_iologic()
{
    std::unordered_map<IdString, BelId> iodelay_to_io;
    std::unordered_map<IdString, XFormRule> iologic_rules;
    iologic_rules[ctx->id("OSERDESE2")].new_type = ctx->id("OSERDESE2_OSERDESE2");

    iologic_rules[ctx->id("ISERDESE2")].new_type = ctx->id("ISERDESE2_ISERDESE2");

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("OSERDESE2")) {
            NetInfo *q = get_net_or_empty(ci, ctx->id("OQ"));
            if (q == nullptr || q->users.empty())
                log_error("%s '%s' has disconnected OQ output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            BelId io_bel;
            if (q->users.size() == 1 && q->users.at(0).cell->type.str(ctx).find("OUTBUF") != std::string::npos)
                io_bel = ctx->getBelByName(ctx->id(q->users.at(0).cell->attrs.at(ctx->id("BEL")).as_string()));
            else if (q->users.size() == 1 && q->users.at(0).cell->type == ctx->id("ODELAYE2") &&
                     q->users.at(0).port == ctx->id("ODATAIN"))
                io_bel = iodelay_to_io.at(q->users.at(0).cell->name);
            else
                log_error("%s '%s' has illegal fanout on OQ output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            std::string ol_site = get_ologic_site(ctx->getBelName(io_bel).str(ctx));
            ci->attrs[ctx->id("BEL")] = ol_site + "/OSERDESE2";
        } else if (ci->type == ctx->id("ISERDESE2")) {
            fold_inverter(ci, "CLKB");
            NetInfo *d = get_net_or_empty(ci, ctx->id("D"));
            if (d == nullptr || d->driver.cell == nullptr)
                log_error("%s '%s' has disconnected D input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            CellInfo *drv = d->driver.cell;
            BelId io_bel;
            if (drv->type.str(ctx).find("INBUF_EN") != std::string::npos)
                io_bel = ctx->getBelByName(ctx->id(drv->attrs.at(ctx->id("BEL")).as_string()));
            else if (drv->type == ctx->id("IDELAYE2") && d->driver.port == ctx->id("DATAOUT"))
                io_bel = iodelay_to_io.at(drv->name);
            else
                log_error("%s '%s' has D input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                          ctx->nameOf(ci), drv->type.c_str(ctx));
            std::string iol_site = get_ilogic_site(ctx->getBelName(io_bel).str(ctx));
            ci->attrs[ctx->id("BEL")] = iol_site + "/ISERDESE2";
        }
    }

    flush_cells();
    generic_xform(iologic_rules, false);
    flush_cells();
}

NEXTPNR_NAMESPACE_END