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
#include "cells.h"
#include "chain_utils.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "pack.h"
#include "pins.h"

NEXTPNR_NAMESPACE_BEGIN

CellInfo *USPacker::insert_ibufctrl(IdString name, NetInfo *i, NetInfo *o)
{
    auto ibufc = create_cell(ctx, ctx->id("IBUFCTRL"), name);
    connect_port(ctx, i, ibufc.get(), ctx->id("I"));
    connect_port(ctx, o, ibufc.get(), ctx->id("O"));
    CellInfo *ibufc_ptr = ibufc.get();
    new_cells.push_back(std::move(ibufc));
    return ibufc_ptr;
}

CellInfo *USPacker::insert_inbuf(IdString name, NetInfo *pad, NetInfo *o)
{
    auto inbuf = create_cell(ctx, ctx->id("INBUF"), name);
    connect_port(ctx, pad, inbuf.get(), ctx->id("PAD"));
    connect_port(ctx, o, inbuf.get(), ctx->id("O"));
    CellInfo *inbuf_ptr = inbuf.get();
    new_cells.push_back(std::move(inbuf));
    return inbuf_ptr;
}

CellInfo *USPacker::insert_obuf(IdString name, IdString type, NetInfo *i, NetInfo *o, NetInfo *tri)
{
    auto obuf = create_cell(ctx, type, name);
    connect_port(ctx, i, obuf.get(), ctx->id("I"));
    connect_port(ctx, tri, obuf.get(), ctx->id("T"));
    connect_port(ctx, o, obuf.get(), ctx->id("O"));
    CellInfo *obuf_ptr = obuf.get();
    new_cells.push_back(std::move(obuf));
    return obuf_ptr;
}

CellInfo *USPacker::insert_diffinbuf(IdString name, NetInfo *i[2], NetInfo *o)
{
    auto dibuf = create_cell(ctx, ctx->id("DIFFINBUF"), name);
    connect_port(ctx, i[0], dibuf.get(), ctx->id("DIFF_IN_P"));
    connect_port(ctx, i[1], dibuf.get(), ctx->id("DIFF_IN_N"));
    connect_port(ctx, o, dibuf.get(), ctx->id("O"));
    CellInfo *dibuf_ptr = dibuf.get();
    new_cells.push_back(std::move(dibuf));
    return dibuf_ptr;
}

NetInfo *USPacker::invert_net(NetInfo *toinv)
{
    if (toinv == nullptr)
        return nullptr;
    // If net is driven by an inverter, don't double-invert, which could cause problems with timing
    // and IOLOGIC packing
    if (toinv->driver.cell != nullptr && toinv->driver.cell->type == ctx->id("LUT1") &&
        int_or_default(toinv->driver.cell->params, ctx->id("INIT"), 0) == 1) {
        NetInfo *preinv = get_net_or_empty(toinv->driver.cell, ctx->id("I0"));
        // If only one user, also sweep the inversion LUT to avoid packing issues
        if (toinv->users.size() == 1) {
            disconnect_port(ctx, toinv->driver.cell, ctx->id("I0"));
            disconnect_port(ctx, toinv->driver.cell, ctx->id("O"));
            packed_cells.insert(toinv->driver.cell->name);
        }
        return preinv;
    } else {
        std::unique_ptr<NetInfo> inv{new NetInfo};
        IdString inv_name = ctx->id(toinv->name.str(ctx) + "$inverted$" + std::to_string(autoidx++));
        auto lut = create_lut(ctx, inv_name.str(ctx) + "$lut", {toinv}, inv.get(), Property(1));
        NetInfo *inv_ptr = inv.get();
        ctx->nets[inv_name] = std::move(inv);
        return inv_ptr;
    }
}

std::vector<CellInfo *> USPacker::decompose_iob(CellInfo *xil_iob)
{
    bool is_se_ibuf = xil_iob->type == ctx->id("IBUF") || xil_iob->type == ctx->id("IBUF_IBUFDISABLE") ||
                      xil_iob->type == ctx->id("IBUF_INTERMDISABLE") || xil_iob->type == ctx->id("IBUFE3");
    bool is_se_iobuf = xil_iob->type == ctx->id("IOBUF") || xil_iob->type == ctx->id("IOBUF_DCIEN") ||
                       xil_iob->type == ctx->id("IOBUF_INTERMDISABLE") || xil_iob->type == ctx->id("IOBUFE3");
    bool is_se_obuf = xil_iob->type == ctx->id("OBUF") || xil_iob->type == ctx->id("OBUFT");
    if (is_se_ibuf || is_se_iobuf) {
        NetInfo *pad_net = get_net_or_empty(xil_iob, is_se_iobuf ? ctx->id("IO") : ctx->id("I"));
        if (!is_se_iobuf)
            disconnect_port(ctx, xil_iob, ctx->id("I"));
        NetInfo *inb_out = create_internal_net(xil_iob->name, "INBUF_OUT");
        CellInfo *inbuf = insert_inbuf(int_name(xil_iob->name, "IBUF"), pad_net, inb_out);
        // Don't need to check cell type here, as replace_port is a no-op if port doesn't exist
        replace_port(xil_iob, ctx->id("VREF"), inbuf, ctx->id("VREF"));
        replace_port(xil_iob, ctx->id("OSC_EN"), inbuf, ctx->id("OSC_EN"));
        for (int i = 0; i < 3; i++)
            replace_port(xil_iob, ctx->id("OSC[" + std::to_string(i) + "]"), inbuf,
                         ctx->id("OSC[" + std::to_string(i) + "]"));

        NetInfo *top_out = get_net_or_empty(xil_iob, ctx->id("O"));
        CellInfo *ibufctrl = insert_ibufctrl(int_name(xil_iob->name, "IBUFCTRL"), inb_out, top_out);
        replace_port(xil_iob, ctx->id("IBUFDISABLE"), ibufctrl, ctx->id("IBUFDISABLE"));
        if (is_se_iobuf)
            connect_port(ctx, get_net_or_empty(xil_iob, ctx->id("T")), ibufctrl, ctx->id("T"));
    }
    if (is_se_obuf || is_se_iobuf) {
        NetInfo *pad_net = get_net_or_empty(xil_iob, is_se_iobuf ? ctx->id("IO") : ctx->id("O"));
        disconnect_port(ctx, xil_iob, is_se_iobuf ? ctx->id("IO") : ctx->id("O"));
        bool has_dci = xil_iob->type == ctx->id("IOBUF_DCIEN") || xil_iob->type == ctx->id("IOBUFE3");
        CellInfo *obuf =
                insert_obuf(int_name(xil_iob->name, "OBUF"),
                            is_se_iobuf ? (has_dci ? ctx->id("OBUFT_DCIEN") : ctx->id("OBUFT")) : xil_iob->type,
                            get_net_or_empty(xil_iob, ctx->id("I")), pad_net, get_net_or_empty(xil_iob, ctx->id("T")));
        replace_port(xil_iob, ctx->id("DCITERMDISABLE"), obuf, ctx->id("DCITERMDISABLE"));
    }
}

CellInfo *USPacker::create_iobuf(CellInfo *npnr_io, IdString &top_port)
{
    std::unique_ptr<CellInfo> cell;
    CellInfo *tbuf = nullptr;
    if (npnr_io->type == ctx->id("$nextpnr_ibuf")) {
        cell = create_cell(ctx, ctx->id("IBUF"), ctx->id(npnr_io->name.str(ctx) + "$ibuf$"));
        replace_port(npnr_io, ctx->id("O"), cell.get(), ctx->id("O"));
        top_port = ctx->id("I");
    } else if (npnr_io->type == ctx->id("$nextpnr_obuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        NetInfo *donet = npnr_io->ports.at(ctx->id("I")).net;
        tbuf = net_driven_by(
                ctx, donet, [](const Context *ctx, const CellInfo *cell) { return cell->type == ctx->id("$_TBUF_"); },
                ctx->id("Y"));

        if (npnr_io->type == ctx->id("$nextpnr_obuf")) {
            cell = create_cell(ctx, tbuf ? ctx->id("OBUFT") : ctx->id("OBUF"),
                               ctx->id(npnr_io->name.str(ctx) + "$obuf$"));
            top_port = ctx->id("O");
        } else {
            cell = create_cell(ctx, ctx->id("IOBUF"), ctx->id(npnr_io->name.str(ctx) + "$iobuf$"));
            replace_port(npnr_io, ctx->id("O"), cell.get(), ctx->id("O"));
            top_port = ctx->id("IO");
            if (!tbuf) {
                if (get_net_or_empty(npnr_io, ctx->id("I")) == nullptr)
                    connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), cell.get(), ctx->id("T"));
                else
                    connect_port(ctx, ctx->nets[ctx->id("$PACKER_GND_NET")].get(), cell.get(), ctx->id("T"));
            }
        }
        if (tbuf) {
            replace_port(tbuf, ctx->id("A"), cell.get(), ctx->id("I"));
            NetInfo *inv_en = invert_net(get_net_or_empty(tbuf, ctx->id("E")));
            connect_port(ctx, inv_en, cell.get(), ctx->id("T"));
        } else {
            replace_port(npnr_io, ctx->id("I"), cell.get(), ctx->id("I"));
        }
    } else {
        NPNR_ASSERT_FALSE("bad IO buffer type");
    }
    // Rename nets to avoid collisions
    NetInfo *i_net = get_net_or_empty(cell.get(), ctx->id("I"));
    if (i_net)
        rename_net(i_net->name, ctx->id(i_net->name.str(ctx) + "$auto$IOBUF_I$"));
    NetInfo *o_net = get_net_or_empty(cell.get(), ctx->id("O"));
    if (o_net)
        rename_net(o_net->name, ctx->id(o_net->name.str(ctx) + "$auto$IOBUF_O$"));

    CellInfo *iob_ptr = cell.get();
    new_cells.push_back(std::move(cell));
    return iob_ptr;
}

std::pair<CellInfo *, PortRef> USPacker::insert_pad_and_buf(CellInfo *npnr_io)
{
    // Given a nextpnr IO buffer, create a PAD instance and insert an IO buffer if one isn't already present
    std::pair<CellInfo *, PortRef> result;
    auto pad_cell = create_cell(ctx, ctx->id("PAD"), npnr_io->name);
    // Copy IO attributes to pad
    for (auto &attr : npnr_io->attrs)
        pad_cell->attrs[attr.first] = attr.second;
    NetInfo *ionet = nullptr;
    PortRef iobuf;
    iobuf.cell = nullptr;
    if (npnr_io->type == ctx->id("$nextpnr_ibuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        ionet = get_net_or_empty(npnr_io, ctx->id("O"));
        if (ionet != nullptr)
            for (auto &usr : ionet->users)
                if (toplevel_ports.count(usr.cell->type) && toplevel_ports.at(usr.cell->type).count(usr.port)) {
                    if (ionet->users.size() > 1)
                        log_error("IO buffer '%s' is connected to more than a single top level IO pin.\n",
                                  usr.cell->name.c_str(ctx));
                    iobuf = usr;
                }
    }
    if (npnr_io->type == ctx->id("$nextpnr_ibuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        ionet = get_net_or_empty(npnr_io, ctx->id("I"));
        if (ionet != nullptr && ionet->driver.cell != nullptr)
            if (toplevel_ports.count(ionet->driver.cell->type) &&
                toplevel_ports.at(ionet->driver.cell->type).count(ionet->driver.port)) {
                if (ionet->users.size() > 1)
                    log_error("IO buffer '%s' is connected to more than a single top level IO pin.\n",
                              ionet->driver.cell->name.c_str(ctx));
                iobuf = ionet->driver;
            }
    }

    if (!iobuf.cell) {
        // No IO buffer, need to create one
        iobuf.cell = create_iobuf(npnr_io, iobuf.port);
        std::unique_ptr<NetInfo> pad_ionet{new NetInfo};
        pad_ionet->name = npnr_io->name;
        NPNR_ASSERT(!ctx->nets.count(pad_ionet->name));
        ionet = pad_ionet.get();
        ctx->nets[npnr_io->name] = std::move(pad_ionet);
    }

    NPNR_ASSERT(ionet != nullptr);

    for (auto &port : npnr_io->ports)
        disconnect_port(ctx, npnr_io, port.first);

    connect_port(ctx, ionet, pad_cell.get(), ctx->id("PAD"));

    result.first = pad_cell.get();
    result.second = iobuf;
    packed_cells.insert(npnr_io->name);
    new_cells.push_back(std::move(pad_cell));
    return result;
}

void USPacker::pack_io()
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

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id_IOB_IBUFCTRL || ci->type == id_IOB_OUTBUF) {
            if (ci->attrs.count(ctx->id("LOC"))) {
                std::string loc = ci->attrs.at(ctx->id("LOC"));
                std::string site = ctx->getPackagePinSite(loc);
                if (site.empty())
                    log_error("Unable to constrain IO '%s', device does not have a pin named '%s'\n",
                              ci->name.c_str(ctx), loc.c_str());
                log_info("    Constraining '%s' to site '%s'\n", ci->name.c_str(ctx), site.c_str());
                std::string belname = (ci->type == id_IOB_IBUFCTRL) ? "IBUFCTRL" : "OUTBUF";
                ci->attrs[ctx->id("BEL")].setString(site + "/" + belname);
            }
        }

        if (ci->type == id_IOB_OUTBUF) {
            NetInfo *inet = get_net_or_empty(ci, ctx->id("I"));
            if (inet)
                rename_net(inet->name, ctx->id(inet->name.str(ctx) + "$obuf_I$"));
        } else if (ci->type == id_IOB_IBUFCTRL) {
            NetInfo *onet = get_net_or_empty(ci, ctx->id("O"));
            if (onet)
                rename_net(onet->name, ctx->id(onet->name.str(ctx) + "$ibuf_O$"));
        }
    }
}

NEXTPNR_NAMESPACE_END