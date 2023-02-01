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
    auto ibufc = create_cell(ctx, id_IBUFCTRL, name);
    ibufc->connectPort(id_I, i);
    ibufc->connectPort(id_O, o);
    CellInfo *ibufc_ptr = ibufc.get();
    new_cells.push_back(std::move(ibufc));
    return ibufc_ptr;
}

CellInfo *USPacker::insert_inbuf(IdString name, NetInfo *pad, NetInfo *o)
{
    auto inbuf = create_cell(ctx, id_INBUF, name);
    inbuf->connectPort(id_PAD, pad);
    inbuf->connectPort(id_O, o);
    CellInfo *inbuf_ptr = inbuf.get();
    new_cells.push_back(std::move(inbuf));
    return inbuf_ptr;
}

CellInfo *XilinxPacker::insert_obuf(IdString name, IdString type, NetInfo *i, NetInfo *o, NetInfo *tri)
{
    auto obuf = create_cell(ctx, type, name);
    obuf->connectPort(id_I, i);
    obuf->connectPort(id_T, tri);
    obuf->connectPort(id_O, o);
    CellInfo *obuf_ptr = obuf.get();
    new_cells.push_back(std::move(obuf));
    return obuf_ptr;
}

CellInfo *USPacker::insert_diffinbuf(IdString name, const std::array<NetInfo *, 2> &i, NetInfo *o)
{
    auto dibuf = create_cell(ctx, id_DIFFINBUF, name);
    dibuf->connectPort(id_DIFF_IN_P, i[0]);
    dibuf->connectPort(id_DIFF_IN_N, i[1]);
    dibuf->connectPort(id_O, o);
    CellInfo *dibuf_ptr = dibuf.get();
    new_cells.push_back(std::move(dibuf));
    return dibuf_ptr;
}

CellInfo *XilinxPacker::insert_outinv(IdString name, NetInfo *i, NetInfo *o)
{
    auto inv = create_cell(ctx, id_INV, name);
    inv->connectPort(id_I, i);
    inv->connectPort(id_O, o);
    CellInfo *inv_ptr = inv.get();
    new_cells.push_back(std::move(inv));
    return inv_ptr;
}

NetInfo *XilinxPacker::invert_net(NetInfo *toinv)
{
    if (toinv == nullptr)
        return nullptr;
    // If net is driven by an inverter, don't double-invert, which could cause problems with timing
    // and IOLOGIC packing
    if (toinv->driver.cell != nullptr && toinv->driver.cell->type == id_LUT1 &&
        int_or_default(toinv->driver.cell->params, id_INIT, 0) == 1) {
        NetInfo *preinv = toinv->driver.cell->getPort(id_I0);
        // If only one user, also sweep the inversion LUT to avoid packing issues
        if (toinv->users.entries() == 1) {
            packed_cells.insert(toinv->driver.cell->name);
            toinv->driver.cell->disconnectPort(id_I0);
            toinv->driver.cell->disconnectPort(id_O);
        }
        return preinv;
    } else {
        std::unique_ptr<NetInfo> inv =
                std::make_unique<NetInfo>(ctx->id(toinv->name.str(ctx) + "$inverted$" + std::to_string(autoidx++)));
        auto lut = create_lut(ctx, inv->name.str(ctx) + "$lut", {toinv}, inv.get(), Property(1));
        NetInfo *inv_ptr = inv.get();
        ctx->nets[inv->name] = std::move(inv);
        new_cells.push_back(std::move(lut));
        return inv_ptr;
    }
}

namespace {
static const pool<std::string> pseudo_diff_iotypes = {"DIFF_POD12_DCI",  "DIFF_POD12",        "DIFF_POD10_DCI",
                                                      "DIFF_POD10",      "DIFF_SSTL12_DCI",   "DIFF_SSTL_135_DCI",
                                                      "DIFF_SSTL15_DCI", "DIFF_SSTL18_I_DCI", "DIFF_SSTL12_I",
                                                      "DIFF_SSTL12_II",  "DIFF_HSTL_I_12",    "DIFF_HSTL_I_12_DCI"};
}

void USPacker::decompose_iob(CellInfo *xil_iob, const std::string &iostandard)
{
    bool is_se_ibuf = xil_iob->type.in(id_IBUF, id_IBUF_IBUFDISABLE, id_IBUF_INTERMDISABLE, id_IBUFE3);
    bool is_se_iobuf = xil_iob->type.in(id_IOBUF, id_IOBUF_DCIEN, id_IOBUF_INTERMDISABLE, id_IOBUFE3);
    bool is_se_obuf = xil_iob->type.in(id_OBUF, id_OBUFT);

    auto pad_site = [&](NetInfo *n) {
        for (auto user : n->users)
            if (user.cell->type == id_PAD)
                return ctx->getBelSite(ctx->getBelByNameStr(user.cell->attrs[id_BEL].as_string()));
        NPNR_ASSERT_FALSE(("can't find PAD for net " + n->name.str(ctx)).c_str());
    };

    /*
     * IO primitives in Xilinx are complex "macros" that usually expand to more than one BEL
     * To avoid various nasty bugs (such as auto-transformation by Vivado of dedicated INV primitives to LUT1s), we
     * have to maintain this hierarchy so it can be re-built during DCP conversion in RapidWright
     */
    dict<IdString, PortInfo> orig_ports = xil_iob->ports;
    std::vector<CellInfo *> subcells;

    auto diffinbuf_site = [&](std::string site_p) {
        BelId pad_out_buf = ctx->getBelByNameStr(site_p + "/PADOUT");
        WireId cursor = ctx->getBelPinWire(pad_out_buf, id_OUT);
        while (true) {
            auto pips_dh = ctx->getPipsDownhill(cursor);
            if (!(pips_dh.begin() != pips_dh.end())) {
                auto bp = ctx->getWireBelPins(cursor);
                NPNR_ASSERT(bp.begin() != bp.end());
                return ctx->getBelSite((*bp.begin()).bel);
            }
            cursor = ctx->getPipDstWire(*pips_dh.begin());
        }
    };

    if (is_se_ibuf || is_se_iobuf) {
        NetInfo *pad_net = xil_iob->getPort(is_se_iobuf ? id_IO : id_I);
        NPNR_ASSERT(pad_net != nullptr);
        std::string site = pad_site(pad_net);
        if (!is_se_iobuf)
            xil_iob->disconnectPort(id_I);
        NetInfo *inb_out = create_internal_net(xil_iob->name, "OUT");
        CellInfo *inbuf = insert_inbuf(int_name(xil_iob->name, "INBUF_INST"), pad_net, inb_out);
        inbuf->attrs[id_BEL] = site + "/INBUF";
        // Don't need to check cell type here, as replace_port is a no-op if port doesn't exist
        xil_iob->movePortTo(id_VREF, inbuf, id_VREF);
        xil_iob->movePortTo(id_OSC_EN, inbuf, id_OSC_EN);
        for (int i = 0; i < 4; i++)
            xil_iob->movePortTo(ctx->id("OSC[" + std::to_string(i) + "]"), inbuf,
                                ctx->id("OSC[" + std::to_string(i) + "]"));

        NetInfo *top_out = xil_iob->getPort(id_O);
        xil_iob->disconnectPort(id_O);
        CellInfo *ibufctrl = insert_ibufctrl(int_name(xil_iob->name, "IBUFCTRL_INST"), inb_out, top_out);
        ibufctrl->attrs[id_BEL] = site + "/IBUFCTRL";
        xil_iob->movePortTo(id_IBUFDISABLE, ibufctrl, id_IBUFDISABLE);
        if (is_se_iobuf)
            ibufctrl->connectPort(id_T, xil_iob->getPort(id_T));

        subcells.push_back(inbuf);
        subcells.push_back(ibufctrl);
    }
    if (is_se_obuf || is_se_iobuf) {
        NetInfo *pad_net = xil_iob->getPort(is_se_iobuf ? id_IO : id_O);
        NPNR_ASSERT(pad_net != nullptr);
        std::string site = pad_site(pad_net);
        xil_iob->disconnectPort(is_se_iobuf ? id_IO : id_O);
        bool has_dci = xil_iob->type.in(id_IOBUF_DCIEN, id_IOBUFE3);
        CellInfo *obuf = insert_obuf(
                int_name(xil_iob->name, (is_se_iobuf || xil_iob->type == id_OBUFT) ? "OBUFT" : "OBUF", !is_se_obuf),
                is_se_iobuf ? (has_dci ? id_OBUFT_DCIEN : id_OBUFT) : xil_iob->type, xil_iob->getPort(id_I), pad_net,
                xil_iob->getPort(id_T));
        obuf->attrs[id_BEL] = site + "/OUTBUF";
        xil_iob->movePortTo(id_DCITERMDISABLE, obuf, id_DCITERMDISABLE);
        if (is_se_iobuf)
            subcells.push_back(obuf);
    }

    bool is_diff_ibuf = xil_iob->type.in(id_IBUFDS, id_IBUFDS_INTERMDISABLE, id_IBUFDSE3);
    bool is_diff_out_ibuf =
            xil_iob->type.in(id_IBUFDS_DIFF_OUT, id_IBUFDS_DIFF_OUT_IBUFDISABLE, id_IBUFDS_DIFF_OUT_INTERMDISABLE);
    bool is_diff_iobuf = xil_iob->type.in(id_IOBUFDS, id_IOBUFDS_DCIEN, id_IOBUFDSE3);
    bool is_diff_out_iobuf =
            xil_iob->type.in(id_IOBUFDS_DIFF_OUT, id_IOBUFDS_DIFF_OUT_DCIEN, id_IOBUFDS_DIFF_OUT_INTERMDISABLE);
    bool is_diff_obuf = xil_iob->type.in(id_OBUFDS, id_OBUFTDS);
    bool is_pseudo_diff_out = pseudo_diff_iotypes.count(iostandard);

    if (is_diff_ibuf || is_diff_out_ibuf || is_diff_iobuf || is_diff_out_iobuf) {
        NetInfo *pad_p_net = xil_iob->getPort((is_diff_iobuf || is_diff_out_iobuf) ? id_IO : id_I);
        NPNR_ASSERT(pad_p_net != nullptr);
        std::string site_p = pad_site(pad_p_net);
        NetInfo *pad_n_net = xil_iob->getPort((is_diff_iobuf || is_diff_out_iobuf) ? id_IOB : id_IB);
        NPNR_ASSERT(pad_n_net != nullptr);
        std::string site_n = pad_site(pad_n_net);

        if (!is_diff_iobuf && !is_diff_out_iobuf) {
            xil_iob->disconnectPort(id_I);
            xil_iob->disconnectPort(id_IB);
        }

        std::string site_dibuf = diffinbuf_site(site_p);
        NetInfo *dibuf_out = create_internal_net(xil_iob->name, "OUT");
        CellInfo *dibuf =
                insert_diffinbuf(int_name(xil_iob->name, "DIFFINBUF_INST"), {pad_p_net, pad_n_net}, dibuf_out);
        dibuf->attrs[id_BEL] = site_dibuf + "/DIFFINBUF";
        for (int i = 0; i < 2; i++)
            xil_iob->movePortTo(ctx->id("OSC_EN[" + std::to_string(i) + "]"), dibuf,
                                ctx->id("OSC_EN[" + std::to_string(i) + "]"));
        for (int i = 0; i < 4; i++)
            xil_iob->movePortTo(ctx->id("OSC[" + std::to_string(i) + "]"), dibuf,
                                ctx->id("OSC[" + std::to_string(i) + "]"));
        xil_iob->movePortTo(id_VREF, dibuf, id_VREF);

        NetInfo *top_out = xil_iob->getPort(id_O);
        xil_iob->disconnectPort(id_O);
        CellInfo *ibufctrl_p = insert_ibufctrl(int_name(xil_iob->name, "IBUFCTRL_INST"), dibuf_out, top_out);
        ibufctrl_p->attrs[id_BEL] = site_p + "/IBUFCTRL";

        subcells.push_back(dibuf);
        subcells.push_back(ibufctrl_p);

        if (is_diff_out_ibuf || is_diff_out_iobuf) {
            NetInfo *dibuf_out_b = create_internal_net(xil_iob->name, "OUTB");
            dibuf->connectPort(id_O_B, dibuf_out_b);
            NetInfo *top_out_b = xil_iob->getPort(id_OB);
            xil_iob->disconnectPort(id_OB);
            CellInfo *ibufctrl_n = insert_ibufctrl(int_name(xil_iob->name, "IBUFCTRLN_INST"), dibuf_out_b, top_out_b);
            ibufctrl_n->attrs[id_BEL] = site_n + "/IBUFCTRL";
            subcells.push_back(ibufctrl_n);
        }
    }

    if (is_diff_obuf || is_diff_out_iobuf || is_diff_iobuf) {
        if (is_pseudo_diff_out) {
            NetInfo *pad_p_net = xil_iob->getPort((is_diff_iobuf || is_diff_out_iobuf) ? id_IO : id_O);
            NPNR_ASSERT(pad_p_net != nullptr);
            std::string site_p = pad_site(pad_p_net);
            NetInfo *pad_n_net = xil_iob->getPort((is_diff_iobuf || is_diff_out_iobuf) ? id_IOB : id_OB);
            NPNR_ASSERT(pad_n_net != nullptr);
            std::string site_n = pad_site(pad_n_net);

            xil_iob->disconnectPort((is_diff_iobuf || is_diff_out_iobuf) ? id_IO : id_O);
            xil_iob->disconnectPort((is_diff_iobuf || is_diff_out_iobuf) ? id_IOB : id_OB);

            NetInfo *inv_i = create_internal_net(xil_iob->name, is_diff_obuf ? "I_B" : "OBUFTDS$subnet$I_B");
            CellInfo *inv = insert_outinv(int_name(xil_iob->name, is_diff_obuf ? "INV" : "OBUFTDS$subcell$INV"),
                                          xil_iob->getPort(id_I), inv_i);
            inv->attrs[id_BEL] = site_p + "/OUTINV";

            bool has_dci = xil_iob->type.in(id_IOBUFDS_DCIEN, id_IOBUFDSE3);

            CellInfo *obuf_p =
                    insert_obuf(int_name(xil_iob->name, is_diff_obuf ? "P" : "OBUFTDS$subcell$P"),
                                (is_diff_iobuf || is_diff_out_iobuf) ? (has_dci ? id_OBUFT_DCIEN : id_OBUFT) : id_OBUF,
                                xil_iob->getPort(id_I), pad_p_net, xil_iob->getPort(id_T));

            obuf_p->attrs[id_BEL] = site_p + "/OUTBUF";
            subcells.push_back(obuf_p);
            obuf_p->connectPort(id_DCITERMDISABLE, xil_iob->getPort(id_DCITERMDISABLE));

            CellInfo *obuf_n =
                    insert_obuf(int_name(xil_iob->name, is_diff_obuf ? "N" : "OBUFTDS$subcell$N"),
                                (is_diff_iobuf || is_diff_out_iobuf) ? (has_dci ? id_OBUFT_DCIEN : id_OBUFT) : id_OBUF,
                                inv_i, pad_n_net, xil_iob->getPort(id_T));

            obuf_n->attrs[id_BEL] = site_n + "/OUTBUF";
            obuf_n->connectPort(id_DCITERMDISABLE, xil_iob->getPort(id_DCITERMDISABLE));

            xil_iob->disconnectPort(id_DCITERMDISABLE);

            subcells.push_back(inv);
            subcells.push_back(obuf_p);
            subcells.push_back(obuf_n);
        } else {
            NPNR_ASSERT_FALSE("true diff obuf not yet implemented");
        }
    }

    if (!subcells.empty()) {
        for (auto sc : subcells) {
            sc->attrs[id_X_ORIG_MACRO_PRIM] = xil_iob->type.str(ctx);
            for (auto &p : sc->ports) {
                std::string macro_ports;
                for (auto &orig : orig_ports) {
                    if ((orig.second.net != nullptr) && (orig.second.net == p.second.net)) {
                        macro_ports += orig.first.str(ctx);
                        macro_ports += ',';
                        macro_ports += (orig.second.type == PORT_INOUT) ? "inout"
                                       : (orig.second.type == PORT_OUT) ? "out"
                                                                        : "in";
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

CellInfo *XilinxPacker::create_iobuf(CellInfo *npnr_io, IdString &top_port)
{
    std::unique_ptr<CellInfo> cell;
    CellInfo *tbuf = nullptr;
    if (npnr_io->type == ctx->id("$nextpnr_ibuf")) {
        cell = create_cell(ctx, id_IBUF, ctx->id(npnr_io->name.str(ctx) + "$ibuf$"));
        npnr_io->movePortTo(id_O, cell.get(), id_O);
        top_port = id_I;
    } else if (npnr_io->type == ctx->id("$nextpnr_obuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        NetInfo *donet = npnr_io->ports.at(id_I).net;
        tbuf = net_driven_by(
                ctx, donet, [](const Context *ctx, const CellInfo *cell) { return cell->type == ctx->id("$_TBUF_"); },
                id_Y);

        if (npnr_io->type == ctx->id("$nextpnr_obuf")) {
            cell = create_cell(ctx, tbuf ? id_OBUFT : id_OBUF, ctx->id(npnr_io->name.str(ctx) + "$obuf$"));
            top_port = id_O;
        } else {
            cell = create_cell(ctx, id_IOBUF, ctx->id(npnr_io->name.str(ctx) + "$iobuf$"));
            npnr_io->movePortTo(id_O, cell.get(), id_O);
            top_port = id_IO;
            if (!tbuf) {
                if (npnr_io->getPort(id_I) == nullptr)
                    cell->connectPort(id_T, ctx->nets[ctx->id("$PACKER_VCC_NET")].get());
                else
                    cell->connectPort(id_T, ctx->nets[ctx->id("$PACKER_GND_NET")].get());
            }
        }
        if (tbuf) {
            log_info("    Replacing %s '%s' with a tristate IO buffer\n", tbuf->type.c_str(ctx), tbuf->name.c_str(ctx));
            tbuf->movePortTo(id_A, cell.get(), id_I);
            NetInfo *inv_en = invert_net(tbuf->getPort(id_E));
            cell->connectPort(id_T, inv_en);

            ctx->nets.erase(tbuf->ports.at(id_Y).net->name);
            tbuf->ports.at(id_Y).net = nullptr;
            packed_cells.insert(tbuf->name);
        } else {
            npnr_io->movePortTo(id_I, cell.get(), id_I);
        }
    } else {
        NPNR_ASSERT_FALSE("bad IO buffer type");
    }
    // Rename nets to avoid collisions
    NetInfo *i_net = cell->getPort(id_I);
    if (i_net)
        rename_net(i_net->name, ctx->id(i_net->name.str(ctx) + "$auto$IOBUF_I$"));
    NetInfo *o_net = cell->getPort(id_O);
    if (o_net)
        rename_net(o_net->name, ctx->id(o_net->name.str(ctx) + "$auto$IOBUF_O$"));

    CellInfo *iob_ptr = cell.get();
    new_cells.push_back(std::move(cell));
    return iob_ptr;
}

std::pair<CellInfo *, PortRef> XilinxPacker::insert_pad_and_buf(CellInfo *npnr_io)
{
    // Given a nextpnr IO buffer, create a PAD instance and insert an IO buffer if one isn't already present
    std::pair<CellInfo *, PortRef> result;
    auto pad_cell = create_cell(ctx, id_PAD, npnr_io->name);
    // Copy IO attributes to pad
    for (auto &attr : npnr_io->attrs)
        pad_cell->attrs[attr.first] = attr.second;
    NetInfo *ionet = nullptr;
    PortRef iobuf;
    iobuf.cell = nullptr;
    if (npnr_io->type == ctx->id("$nextpnr_obuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        ionet = npnr_io->getPort(id_I);
        if (ionet != nullptr && ionet->driver.cell != nullptr)
            if (toplevel_ports.count(ionet->driver.cell->type) &&
                toplevel_ports.at(ionet->driver.cell->type).count(ionet->driver.port)) {
                if (ionet->users.entries() > 1)
                    log_error("IO buffer '%s' is connected to more than a single top level IO pin.\n",
                              ionet->driver.cell->name.c_str(ctx));
                iobuf = ionet->driver;
            }
        pad_cell->attrs[id_X_IO_DIR] = std::string(npnr_io->type == ctx->id("$nextpnr_obuf") ? "OUT" : "INOUT");
    }
    if (npnr_io->type == ctx->id("$nextpnr_ibuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        ionet = npnr_io->getPort(id_O);
        if (ionet != nullptr)
            for (auto &usr : ionet->users)
                if (toplevel_ports.count(usr.cell->type) && toplevel_ports.at(usr.cell->type).count(usr.port)) {
                    if (ionet->users.entries() > 1)
                        log_error("IO buffer '%s' is connected to more than a single top level IO pin.\n",
                                  usr.cell->name.c_str(ctx));
                    iobuf = usr;
                }
        pad_cell->attrs[id_X_IO_DIR] = std::string(npnr_io->type == ctx->id("$nextpnr_ibuf") ? "IN" : "INOUT");
    }

    if (!iobuf.cell) {
        // No IO buffer, need to create one
        iobuf.cell = create_iobuf(npnr_io, iobuf.port);
        std::unique_ptr<NetInfo> pad_ionet = std::make_unique<NetInfo>(npnr_io->name);
        NPNR_ASSERT(!ctx->nets.count(pad_ionet->name));
        ionet = pad_ionet.get();
        ctx->nets[npnr_io->name] = std::move(pad_ionet);
    } else {
        log_info("    IO port '%s' driven by %s '%s'\n", npnr_io->name.c_str(ctx), iobuf.cell->type.c_str(ctx),
                 iobuf.cell->name.c_str(ctx));
    }

    NPNR_ASSERT(ionet != nullptr);

    for (auto &port : npnr_io->ports)
        npnr_io->disconnectPort(port.first);

    pad_cell->connectPort(id_PAD, ionet);
    if (iobuf.cell->ports.at(iobuf.port).net != ionet) {
        iobuf.cell->disconnectPort(iobuf.port);
        iobuf.cell->connectPort(iobuf.port, ionet);
    }

    result.first = pad_cell.get();
    result.second = iobuf;
    packed_cells.insert(npnr_io->name);
    new_cells.push_back(std::move(pad_cell));
    return result;
}

void USPacker::pack_io()
{
    log_info("Inserting IO buffers..\n");

    get_top_level_pins(ctx, toplevel_ports);
    // Insert PAD cells on top level IO, and IO buffers where one doesn't exist already
    std::vector<std::pair<CellInfo *, PortRef>> pad_and_buf;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == ctx->id("$nextpnr_ibuf") || ci->type == ctx->id("$nextpnr_iobuf") ||
            ci->type == ctx->id("$nextpnr_obuf"))
            pad_and_buf.push_back(insert_pad_and_buf(ci));
    }
    flush_cells();
    pool<BelId> used_io_bels;
    int unconstr_io_count = 0;
    for (auto &iob : pad_and_buf) {
        CellInfo *pad = iob.first;
        // Process location constraints
        if (pad->attrs.count(id_LOC)) {
            std::string loc = pad->attrs.at(id_LOC).as_string();
            std::string site = ctx->getPackagePinSite(loc);
            if (site.empty())
                log_error("Unable to constrain IO '%s', device does not have a pin named '%s'\n", pad->name.c_str(ctx),
                          loc.c_str());
            log_info("    Constraining '%s' to site '%s'\n", pad->name.c_str(ctx), site.c_str());
            pad->attrs[id_BEL] = std::string(site + "/PAD");
        }
        if (pad->attrs.count(id_BEL)) {
            used_io_bels.insert(ctx->getBelByNameStr(pad->attrs.at(id_BEL).as_string()));
        } else {
            ++unconstr_io_count;
        }
    }
    std::queue<BelId> available_io_bels;
    IdString pad_id = id_IOB_PAD;
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
        if (!pad->attrs.count(id_BEL)) {
            pad->attrs[id_BEL] = std::string(ctx->nameOfBel(available_io_bels.front()));
            available_io_bels.pop();
        }
    }
    // Decompose macro IO primitives to smaller primitives that map logically to the actual IO Bels
    for (auto &iob : pad_and_buf) {
        if (packed_cells.count(iob.second.cell->name))
            continue;
        decompose_iob(iob.second.cell, str_or_default(iob.first->attrs, id_IOSTANDARD, ""));
        packed_cells.insert(iob.second.cell->name);
    }
    flush_cells();

    // Type transformations from logical to physical
    dict<IdString, XFormRule> io_rules;
    io_rules[id_PAD].new_type = id_IOB_PAD;
    io_rules[id_OBUF].new_type = id_IOB_OUTBUF;
    io_rules[id_OBUFT].new_type = id_IOB_OUTBUF;
    io_rules[id_OBUFT].port_xform[id_T] = id_TRI;
    io_rules[id_OBUFT_DCIEN].new_type = id_IOB_OUTBUF;
    io_rules[id_OBUFT_DCIEN].port_xform[id_T] = id_TRI;
    io_rules[id_INBUF].new_type = id_IOB_INBUF;
    io_rules[id_IBUFCTRL].new_type = id_IOB_IBUFCTRL;
    io_rules[id_IBUFCTRL].port_xform[id_T] = id_TRI;
    io_rules[id_DIFFINBUF].new_type = id_IOB_DIFFINBUF;
    io_rules[id_INV].new_type = id_HPIO_OUTINV;
    io_rules[id_INV].port_xform[id_I] = id_IN;
    io_rules[id_INV].port_xform[id_O] = id_OUT;

    io_rules[id_PS8].new_type = id_PSS_ALTO_CORE;

    generic_xform(io_rules, true);
}

std::string USPacker::get_iol_site(const std::string &io_bel)
{
    BelId ibc_bel = ctx->getBelByNameStr(io_bel.substr(0, io_bel.find('/')) + "/IBUFCTRL");
    WireId start = ctx->getBelPinWire(ibc_bel, id_O);
    WireId cursor = start;
    while (true) {
        auto bp = ctx->getWireBelPins(cursor);
        if (cursor != start && bp.begin() != bp.end()) {
            return ctx->getBelSite((*bp.begin()).bel);
        }
        auto pips_dh = ctx->getPipsDownhill(cursor);
        NPNR_ASSERT(pips_dh.begin() != pips_dh.end());
        cursor = ctx->getPipDstWire(*pips_dh.begin());
    }
}

std::string USPacker::get_ioctrl_site(const std::string &iol_bel)
{
    BelId ibc_bel = ctx->getBelByNameStr(iol_bel.substr(0, iol_bel.find('/')) + "/RXTX_BITSLICE");
    WireId start = ctx->getBelPinWire(ibc_bel, id_TX_BIT_CTRL_OUT0);
    WireId cursor = start;
    while (true) {
        auto bp = ctx->getWireBelPins(cursor);
        if (cursor != start && bp.begin() != bp.end()) {
            return ctx->getBelSite((*bp.begin()).bel);
        }
        auto pips_dh = ctx->getPipsDownhill(cursor);
        NPNR_ASSERT(pips_dh.begin() != pips_dh.end());
        cursor = ctx->getPipDstWire(*pips_dh.begin());
    }
}

void USPacker::prepare_iologic()
{
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        // ODDRE1 must be transformed to an OSERDESE3
        if (ci->type == id_ODDRE1) {
            ci->type = id_OSERDESE3;
            ci->params[id_ODDR_MODE] = std::string("TRUE");
            ci->renamePort(id_C, id_CLK);
            ci->renamePort(id_SR, id_RST);
            ci->renamePort(id_D1, ctx->id("D[0]"));
            ci->renamePort(id_D2, ctx->id("D[4]"));
            ci->renamePort(id_Q, id_OQ);
        }
    }
}

void USPacker::pack_iologic()
{
    hd_iol_rules[id_IDDRE1].new_type = id_IOL_IDDR;
    hd_iol_rules[id_IDDRE1].port_xform[id_C] = id_CK;
    hd_iol_rules[id_IDDRE1].port_xform[id_CB] = id_CK_C;
    hd_iol_rules[id_IDDRE1].port_xform[id_R] = id_RST;

    hd_iol_rules[id_OSERDESE3].new_type = id_IOL_OPTFF;
    hd_iol_rules[id_OSERDESE3].port_xform[id_CLK] = id_CK;
    hd_iol_rules[id_OSERDESE3].port_xform[ctx->id("D[0]")] = id_D1;
    hd_iol_rules[id_OSERDESE3].port_xform[ctx->id("D[4]")] = id_D2;

    hp_iol_rules[id_IDDRE1].new_type = id_ISERDESE3;
    hp_iol_rules[id_IDDRE1].port_xform[id_C] = id_CLK;
    hp_iol_rules[id_IDDRE1].port_xform[id_CB] = id_CLK_B;
    hp_iol_rules[id_IDDRE1].port_xform[id_R] = id_RST;
    hp_iol_rules[id_IDDRE1].port_xform[id_Q1] = id_Q0;
    hp_iol_rules[id_IDDRE1].port_xform[id_Q2] = id_Q1;

    hp_iol_rules[id_OSERDESE3].new_type = id_OSERDESE3;
    hp_iol_rules[id_ISERDESE3].new_type = id_ISERDESE3;

    hp_iol_rules[id_ODELAYE3].new_type = id_ODELAYE3;
    hp_iol_rules[id_IDELAYE3].new_type = id_IDELAYE3;

    auto is_hpio = [&](BelId bel) { return ctx->getBelTileType(bel).in(id_HPIO_L, id_HPIO_RIGHT); };

    dict<IdString, BelId> iodelay_to_io;

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_IDELAYE3) {
            NetInfo *d = ci->getPort(id_IDATAIN);
            if (d == nullptr || d->driver.cell == nullptr)
                log_error("%s '%s' has disconnected IDATAIN input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            CellInfo *drv = d->driver.cell;
            BelId io_bel;
            if (drv->type == id_IOB_IBUFCTRL)
                io_bel = ctx->getBelByNameStr(drv->attrs.at(id_BEL).as_string());
            else
                log_error("%s '%s' has D input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                          ctx->nameOf(ci), drv->type.c_str(ctx));
            std::string iol_site = get_iol_site(ctx->getBelName(io_bel).str(ctx));
            if (is_hpio(io_bel)) {
                ci->attrs[id_BEL] = iol_site + "/IDELAY";
                iodelay_to_io[ci->name] = io_bel;
                xform_cell(hp_iol_rules, ci);
            } else {
                log_error("%s '%s' cannot be placed in a HDIO site\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            }
        } else if (ci->type == id_ODELAYE3) {
            NetInfo *q = ci->getPort(id_DATAOUT);
            if (q == nullptr || q->users.empty())
                log_error("%s '%s' has disconnected DATAOUT output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            BelId io_bel;
            for (auto &usr : q->users) {
                // log_info("%s %s %s.%s [%s]\n", ctx->nameOf(ci), ctx->nameOf(q), ctx->nameOf(usr.cell),
                // usr.port.c_str(ctx), usr.cell->type.c_str(ctx));
                if (usr.cell->type == id_IOB_OUTBUF) {
                    io_bel = ctx->getBelByNameStr(usr.cell->attrs.at(id_BEL).as_string());
                    break;
                }
            }
            if (io_bel == BelId())
                log_error("%s '%s' has no top level output buffer connected to DATAOUT output\n", ci->type.c_str(ctx),
                          ctx->nameOf(ci));
            std::string iol_site = get_iol_site(ctx->getBelName(io_bel).str(ctx));
            if (is_hpio(io_bel)) {
                ci->attrs[id_BEL] = iol_site + "/ODELAY";
                iodelay_to_io[ci->name] = io_bel;
                xform_cell(hp_iol_rules, ci);
            } else {
                log_error("%s '%s' cannot be placed in a HDIO site\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            }
        }
    }

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type.in(id_IDDRE1, id_ISERDESE3)) {
            NetInfo *d = ci->getPort(id_D);
            if (d == nullptr || d->driver.cell == nullptr)
                log_error("%s '%s' has disconnected D input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            CellInfo *drv = d->driver.cell;
            BelId io_bel;
            if (drv->type == id_IOB_IBUFCTRL)
                io_bel = ctx->getBelByNameStr(drv->attrs.at(id_BEL).as_string());
            else if (drv->type == id_IDELAYE3 && d->driver.port == id_DATAOUT)
                io_bel = iodelay_to_io.at(drv->name);
            else
                log_error("%s '%s' has D input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                          ctx->nameOf(ci), drv->type.c_str(ctx));
            std::string iol_site = get_iol_site(ctx->getBelName(io_bel).str(ctx));
            if (is_hpio(io_bel)) {
                xform_cell(hp_iol_rules, ci);
                ci->attrs[id_BEL] = iol_site + "/ISERDES";
                if (ci->getPort(id_IFD_CE) == nullptr) {
                    ci->ports[id_IFD_CE].name = id_IFD_CE;
                    ci->ports[id_IFD_CE].type = PORT_IN;
                    ci->connectPort(id_IFD_CE, ctx->nets[ctx->id("$PACKER_GND_NET")].get());
                }
            } else {
                if (ci->type == id_ISERDESE3)
                    log_error("%s '%s' cannot be placed in a HDIO site\n", ci->type.c_str(ctx), ctx->nameOf(ci));
                xform_cell(hd_iol_rules, ci);
                ci->attrs[id_BEL] = iol_site + "/IDDR";
            }
        } else if (ci->type == id_OSERDESE3) {
            NetInfo *q = ci->getPort(id_OQ);
            if (q == nullptr || q->users.empty())
                log_error("%s '%s' has disconnected OQ output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            BelId io_bel;
            if (q->users.entries() == 1 && (*q->users.begin()).cell->type == id_IOB_OUTBUF)
                io_bel = ctx->getBelByNameStr((*q->users.begin()).cell->attrs.at(id_BEL).as_string());
            else if (q->users.entries() == 1 && (*q->users.begin()).cell->type == id_ODELAYE3 &&
                     (*q->users.begin()).port == id_ODATAIN)
                io_bel = iodelay_to_io.at((*q->users.begin()).cell->name);
            else
                log_error("%s '%s' has illegal fanout on OQ output\n", ci->type.c_str(ctx), ctx->nameOf(ci));

            std::string iol_site = get_iol_site(ctx->getBelName(io_bel).str(ctx));
            if (is_hpio(io_bel)) {
                NetInfo *rst = ci->getPort(id_RST);
                if (rst == ctx->nets[ctx->id("$PACKER_GND_NET")].get()) {
                    // Can't use the general invertible_pins framework here as this only applies
                    // to HPIO locations
                    ci->disconnectPort(id_RST);
                    ci->connectPort(id_RST, ctx->nets[ctx->id("$PACKER_VCC_NET")].get());
                    ci->params[id_IS_RST_INVERTED] = 1;
                }

                xform_cell(hp_iol_rules, ci);
                ci->attrs[id_BEL] = iol_site + "/OSERDES";
                if (ci->getPort(id_OFD_CE) == nullptr) {
                    ci->ports[id_OFD_CE].name = id_OFD_CE;
                    ci->ports[id_OFD_CE].type = PORT_IN;
                    ci->connectPort(id_OFD_CE, ctx->nets[ctx->id("$PACKER_GND_NET")].get());
                }

            } else {
                ci->disconnectPort(id_T);
                xform_cell(hd_iol_rules, ci);
                ci->attrs[id_BEL] = iol_site + "/OPTFF";
            }
        }
    }
}

void USPacker::pack_idelayctrl()
{
    CellInfo *idelayctrl = nullptr;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_IDELAYCTRL) {
            if (idelayctrl != nullptr)
                log_error("Found more than one IDELAYCTRL cell!\n");
            idelayctrl = ci;
        }
    }
    if (idelayctrl == nullptr)
        return;
    std::set<std::string> ioctrl_sites;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type.in(id_IDELAYE3, id_ODELAYE3)) {
            if (!ci->attrs.count(id_BEL))
                continue;
            ioctrl_sites.insert(get_ioctrl_site(ci->attrs.at(id_BEL).as_string()));
        }
    }
    if (ioctrl_sites.empty())
        log_error("Found IDELAYCTRL but no I/ODELAYs\n");
    NetInfo *rdy = idelayctrl->getPort(id_RDY);
    idelayctrl->disconnectPort(id_RDY);
    std::vector<NetInfo *> dup_rdys;
    int i = 0;
    for (auto site : ioctrl_sites) {
        auto dup_idc =
                create_cell(ctx, id_IDELAYCTRL, int_name(idelayctrl->name, "CTRL_DUP_" + std::to_string(i), false));
        dup_idc->connectPort(id_REFCLK, idelayctrl->getPort(id_REFCLK));
        dup_idc->connectPort(id_RST, idelayctrl->getPort(id_RST));
        if (rdy != nullptr) {
            NetInfo *dup_rdy =
                    (ioctrl_sites.size() == 1)
                            ? rdy
                            : create_internal_net(idelayctrl->name, "CTRL_DUP_" + std::to_string(i) + "_RDY", false);
            dup_idc->connectPort(id_RDY, dup_rdy);
            dup_rdys.push_back(dup_rdy);
        }
        dup_idc->attrs[id_BEL] = site + "/CONTROL";
        new_cells.push_back(std::move(dup_idc));
        ++i;
    }
    idelayctrl->disconnectPort(id_REFCLK);
    idelayctrl->disconnectPort(id_RST);

    if (rdy != nullptr) {
        // AND together all the RDY signals
        std::vector<NetInfo *> int_anded_rdy;
        int_anded_rdy.push_back(dup_rdys.front());
        for (size_t j = 1; j < dup_rdys.size(); j++) {
            NetInfo *anded_net =
                    (j == (dup_rdys.size() - 1))
                            ? rdy
                            : create_internal_net(idelayctrl->name, "ANDED_RDY_" + std::to_string(j), false);
            auto lut = create_lut(ctx, idelayctrl->name.str(ctx) + "/RDY_AND_LUT_" + std::to_string(j),
                                  {int_anded_rdy.at(j - 1), dup_rdys.at(j)}, anded_net, Property(8));
            int_anded_rdy.push_back(anded_net);
            new_cells.push_back(std::move(lut));
        }
    }

    packed_cells.insert(idelayctrl->name);
    flush_cells();

    ioctrl_rules[id_IDELAYCTRL].new_type = id_BITSLICE_CONTROL_BEL;
    ioctrl_rules[id_IDELAYCTRL].port_xform[id_RDY] = id_VTC_RDY;

    generic_xform(ioctrl_rules);
}

NEXTPNR_NAMESPACE_END