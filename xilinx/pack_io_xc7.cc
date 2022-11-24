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

std::string get_tilename_by_sitename(Context *ctx, std::string site)
{
    if (ctx->site_by_name.count(site)) {
        int tile, siteid;
        std::tie(tile, siteid) = ctx->site_by_name.at(site);
        return ctx->chip_info->tile_insts[tile].name.get();
    }
    return std::string();
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
        log_info("Generating input buffer for '%s'\n", xil_iob->name.c_str(ctx));
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
        std::string tile = get_tilename_by_sitename(ctx, site);
        if (boost::starts_with(tile, "RIOB18_"))
            inbuf->attrs[ctx->id("BEL")] = site + "/IOB18/INBUF_DCIEN";
        else
            inbuf->attrs[ctx->id("BEL")] = site + "/IOB33/INBUF_EN";
        replace_port(xil_iob, ctx->id("IBUFDISABLE"), inbuf, ctx->id("IBUFDISABLE"));
        replace_port(xil_iob, ctx->id("INTERMDISABLE"), inbuf, ctx->id("INTERMDISABLE"));

        if (is_se_iobuf)
            subcells.push_back(inbuf);
    }

    if (is_se_obuf || is_se_iobuf) {
        log_info("Generating output buffer for '%s'\n", xil_iob->name.c_str(ctx));
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
        std::string tile = get_tilename_by_sitename(ctx, site);
        if (boost::starts_with(tile, "RIOB18_"))
            obuf->attrs[ctx->id("BEL")] = site + "/IOB18/OUTBUF_DCIEN";
        else
            obuf->attrs[ctx->id("BEL")] = site + "/IOB33/OUTBUF";
        replace_port(xil_iob, ctx->id("DCITERMDISABLE"), obuf, ctx->id("DCITERMDISABLE"));
        if (is_se_iobuf)
            subcells.push_back(obuf);
    }

    bool is_diff_ibuf = xil_iob->type == ctx->id("IBUFDS") || xil_iob->type == ctx->id("IBUFDS_INTERMDISABLE") ||
                        xil_iob->type == ctx->id("IBUFDS");
    bool is_diff_out_ibuf = xil_iob->type == ctx->id("IBUFDS_DIFF_OUT") ||
                            xil_iob->type == ctx->id("IBUFDS_DIFF_OUT_IBUFDISABLE") ||
                            xil_iob->type == ctx->id("IBUFDS_DIFF_OUT_INTERMDISABLE");
    bool is_diff_iobuf = xil_iob->type == ctx->id("IOBUFDS") || xil_iob->type == ctx->id("IOBUFDS_DCIEN");
    bool is_diff_out_iobuf = xil_iob->type == ctx->id("IOBUFDS_DIFF_OUT") ||
                             xil_iob->type == ctx->id("IOBUFDS_DIFF_OUT_DCIEN") ||
                             xil_iob->type == ctx->id("IOBUFDS_DIFF_OUT_INTERMDISABLE");
    bool is_diff_obuf = xil_iob->type == ctx->id("OBUFDS") || xil_iob->type == ctx->id("OBUFTDS");

    if (is_diff_ibuf || is_diff_iobuf) {
        NetInfo *pad_p_net =
                get_net_or_empty(xil_iob, (is_diff_iobuf || is_diff_out_iobuf) ? ctx->id("IO") : ctx->id("I"));
        NPNR_ASSERT(pad_p_net != nullptr);
        std::string site_p = pad_site(pad_p_net);
        NetInfo *pad_n_net =
                get_net_or_empty(xil_iob, (is_diff_iobuf || is_diff_out_iobuf) ? ctx->id("IOB") : ctx->id("IB"));
        NPNR_ASSERT(pad_n_net != nullptr);
        std::string site_n = pad_site(pad_n_net);
        std::string tile_p = get_tilename_by_sitename(ctx, site_p);
        bool is_riob18 = boost::starts_with(tile_p, "RIOB18_");

        if (!is_diff_iobuf && !is_diff_out_iobuf) {
            disconnect_port(ctx, xil_iob, ctx->id("I"));
            disconnect_port(ctx, xil_iob, ctx->id("IB"));
        }

        NetInfo *top_out = get_net_or_empty(xil_iob, ctx->id("O"));
        disconnect_port(ctx, xil_iob, ctx->id("O"));

        IdString ibuf_type = ctx->id("IBUFDS");
        CellInfo *inbuf = insert_diffibuf(int_name(xil_iob->name, "IBUF", is_se_iobuf), ibuf_type,
                                          {pad_p_net, pad_n_net}, top_out);
        if (is_riob18) {
            inbuf->attrs[ctx->id("BEL")] = site_p + "/IOB18M/INBUF_DCIEN";
            inbuf->attrs[ctx->id("X_IOB_SITE_TYPE")] = std::string("IOB18M");
        } else {
            inbuf->attrs[ctx->id("BEL")] = site_p + "/IOB33M/INBUF_EN";
            inbuf->attrs[ctx->id("X_IOB_SITE_TYPE")] = std::string("IOB33M");
        }

        if (is_diff_iobuf)
            subcells.push_back(inbuf);
    }

    if (is_diff_obuf || is_diff_out_iobuf || is_diff_iobuf) {
        // FIXME: true diff outputs
        NetInfo *pad_p_net =
                get_net_or_empty(xil_iob, (is_diff_iobuf || is_diff_out_iobuf) ? ctx->id("IO") : ctx->id("O"));
        NPNR_ASSERT(pad_p_net != nullptr);
        std::string site_p = pad_site(pad_p_net);
        NetInfo *pad_n_net =
                get_net_or_empty(xil_iob, (is_diff_iobuf || is_diff_out_iobuf) ? ctx->id("IOB") : ctx->id("OB"));
        NPNR_ASSERT(pad_n_net != nullptr);
        std::string site_n = pad_site(pad_n_net);
        std::string tile_p = get_tilename_by_sitename(ctx, site_p);
        bool is_riob18 = boost::starts_with(tile_p, "RIOB18_");

        disconnect_port(ctx, xil_iob, (is_diff_iobuf || is_diff_out_iobuf) ? ctx->id("IO") : ctx->id("O"));
        disconnect_port(ctx, xil_iob, (is_diff_iobuf || is_diff_out_iobuf) ? ctx->id("IOB") : ctx->id("OB"));

        NetInfo *inv_i = create_internal_net(xil_iob->name, is_diff_obuf ? "I_B" : "OBUFTDS$subnet$I_B");
        CellInfo *inv = insert_outinv(int_name(xil_iob->name, is_diff_obuf ? "INV" : "OBUFTDS$subcell$INV"),
                                      get_net_or_empty(xil_iob, ctx->id("I")), inv_i);
        if (is_riob18) {
            inv->attrs[ctx->id("BEL")] = site_n + "/IOB18S/O_ININV";
            inv->attrs[ctx->id("X_IOB_SITE_TYPE")] = std::string("IOB18S");
        } else {
            inv->attrs[ctx->id("BEL")] = site_n + "/IOB33S/O_ININV";
            inv->attrs[ctx->id("X_IOB_SITE_TYPE")] = std::string("IOB33S");
        }

        bool has_dci = xil_iob->type == ctx->id("IOBUFDS_DCIEN") || xil_iob->type == ctx->id("IOBUFDSE3");

        CellInfo *obuf_p = insert_obuf(int_name(xil_iob->name, is_diff_obuf ? "P" : "OBUFTDS$subcell$P"),
                                       (is_diff_iobuf || is_diff_out_iobuf || (xil_iob->type == ctx->id("OBUFTDS")))
                                               ? (has_dci ? ctx->id("OBUFT_DCIEN") : ctx->id("OBUFT"))
                                               : ctx->id("OBUF"),
                                       get_net_or_empty(xil_iob, ctx->id("I")), pad_p_net,
                                       get_net_or_empty(xil_iob, ctx->id("T")));

        if (is_riob18) {
            obuf_p->attrs[ctx->id("BEL")] = site_p + "/IOB18M/OUTBUF_DCIEN";
            obuf_p->attrs[ctx->id("X_IOB_SITE_TYPE")] = std::string("IOB18M");
        } else {
            obuf_p->attrs[ctx->id("BEL")] = site_p + "/IOB33M/OUTBUF";
            obuf_p->attrs[ctx->id("X_IOB_SITE_TYPE")] = std::string("IOB33M");
        }
        subcells.push_back(obuf_p);
        connect_port(ctx, get_net_or_empty(xil_iob, ctx->id("DCITERMDISABLE")), obuf_p, ctx->id("DCITERMDISABLE"));

        CellInfo *obuf_n = insert_obuf(int_name(xil_iob->name, is_diff_obuf ? "N" : "OBUFTDS$subcell$N"),
                                       (is_diff_iobuf || is_diff_out_iobuf || (xil_iob->type == ctx->id("OBUFTDS")))
                                               ? (has_dci ? ctx->id("OBUFT_DCIEN") : ctx->id("OBUFT"))
                                               : ctx->id("OBUF"),
                                       inv_i, pad_n_net, get_net_or_empty(xil_iob, ctx->id("T")));

        if (is_riob18) {
            obuf_n->attrs[ctx->id("BEL")] = site_n + "/IOB18S/OUTBUF_DCIEN";
            obuf_n->attrs[ctx->id("X_IOB_SITE_TYPE")] = std::string("IOB18S");
        } else {
            obuf_n->attrs[ctx->id("BEL")] = site_n + "/IOB33S/OUTBUF";
            obuf_n->attrs[ctx->id("X_IOB_SITE_TYPE")] = std::string("IOB33S");
        }
        connect_port(ctx, get_net_or_empty(xil_iob, ctx->id("DCITERMDISABLE")), obuf_n, ctx->id("DCITERMDISABLE"));

        disconnect_port(ctx, xil_iob, ctx->id("DCITERMDISABLE"));

        subcells.push_back(inv);
        subcells.push_back(obuf_p);
        subcells.push_back(obuf_n);
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

void XC7Packer::pack_io()
{
    // make sure the supporting data structure of
    // get_tilename_by_sitename()
    // is initialized before we use it below
    ctx->setup_byname();

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
        if (pad->attrs.count(ctx->id("PACKAGE_PIN"))) {
            pad->attrs[ctx->id("LOC")] = pad->attrs.at(ctx->id("PACKAGE_PIN"));
        }
        if (pad->attrs.count(ctx->id("LOC"))) {
            std::string loc = pad->attrs.at(ctx->id("LOC")).as_string();
            std::string site = ctx->getPackagePinSite(loc);
            if (site.empty())
                log_error("Unable to constrain IO '%s', device does not have a pin named '%s'\n", pad->name.c_str(ctx),
                          loc.c_str());
            log_info("    Constraining '%s' to site '%s'\n", pad->name.c_str(ctx), site.c_str());
            std::string tile = get_tilename_by_sitename(ctx, site);
            log_info("    Tile '%s'\n", tile.c_str());
            if (boost::starts_with(tile, "RIOB18_"))
                pad->attrs[ctx->id("BEL")] = std::string(site + "/IOB18/PAD");
            else
                pad->attrs[ctx->id("BEL")] = std::string(site + "/IOB33/PAD");
        }
        if (pad->attrs.count(ctx->id("BEL"))) {
            used_io_bels.insert(ctx->getBelByName(ctx->id(pad->attrs.at(ctx->id("BEL")).as_string())));
        } else {
            ++unconstr_io_count;
        }
    }
    std::queue<BelId> available_io_bels;
    IdString pad_id = ctx->xc7 ? ctx->id("PAD") : ctx->id("IOB_PAD");
    for (auto bel : ctx->getBels()) {
        if (int(available_io_bels.size()) >= unconstr_io_count)
            break;
        if (ctx->locInfo(bel).bel_data[bel.index].site_variant != 0)
            continue;
        if (ctx->getBelType(bel) != pad_id)
            continue;
        if (ctx->getBelPackagePin(bel) == ".")
            continue;
        if (used_io_bels.count(bel))
            continue;
        available_io_bels.push(bel);
    }
    int avail_count = int(available_io_bels.size());
    // Constrain unconstrained IO
    for (auto &iob : pad_and_buf) {
        CellInfo *pad = iob.first;
        if (!pad->attrs.count(ctx->id("BEL"))) {
            if (available_io_bels.empty()) {
                log_error("IO placer ran out of available IOs (%d available IO, %d unconstrained pins)\n", avail_count,
                          unconstr_io_count);
            }
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

    std::unordered_map<IdString, XFormRule> hriobuf_rules, hpiobuf_rules;
    hriobuf_rules[ctx->id("OBUF")].new_type = ctx->id("IOB33_OUTBUF");
    hriobuf_rules[ctx->id("OBUF")].port_xform[ctx->id("I")] = ctx->id("IN");
    hriobuf_rules[ctx->id("OBUF")].port_xform[ctx->id("O")] = ctx->id("OUT");
    hriobuf_rules[ctx->id("OBUF")].port_xform[ctx->id("T")] = ctx->id("TRI");
    hriobuf_rules[ctx->id("OBUFT")] = hriobuf_rules[ctx->id("OBUF")];

    hriobuf_rules[ctx->id("IBUF")].new_type = ctx->id("IOB33_INBUF_EN");
    hriobuf_rules[ctx->id("IBUF")].port_xform[ctx->id("I")] = ctx->id("PAD");
    hriobuf_rules[ctx->id("IBUF")].port_xform[ctx->id("O")] = ctx->id("OUT");
    hriobuf_rules[ctx->id("IBUF_INTERMDISABLE")] = hriobuf_rules[ctx->id("IBUF")];
    hriobuf_rules[ctx->id("IBUF_IBUFDISABLE")] = hriobuf_rules[ctx->id("IBUF")];
    hriobuf_rules[ctx->id("IBUFDS_INTERMDISABLE_INT")] = hriobuf_rules[ctx->id("IBUF")];
    hriobuf_rules[ctx->id("IBUFDS_INTERMDISABLE_INT")].port_xform[ctx->id("IB")] = ctx->id("DIFFI_IN");
    hriobuf_rules[ctx->id("IBUFDS")] = hriobuf_rules[ctx->id("IBUF")];
    hriobuf_rules[ctx->id("IBUFDS")].port_xform[ctx->id("IB")] = ctx->id("DIFFI_IN");

    hpiobuf_rules[ctx->id("OBUF")].new_type = ctx->id("IOB18_OUTBUF_DCIEN");
    hpiobuf_rules[ctx->id("OBUF")].port_xform[ctx->id("I")] = ctx->id("IN");
    hpiobuf_rules[ctx->id("OBUF")].port_xform[ctx->id("O")] = ctx->id("OUT");
    hpiobuf_rules[ctx->id("OBUF")].port_xform[ctx->id("T")] = ctx->id("TRI");
    hpiobuf_rules[ctx->id("OBUFT")] = hpiobuf_rules[ctx->id("OBUF")];

    hpiobuf_rules[ctx->id("IBUF")].new_type = ctx->id("IOB18_INBUF_DCIEN");
    hpiobuf_rules[ctx->id("IBUF")].port_xform[ctx->id("I")] = ctx->id("PAD");
    hpiobuf_rules[ctx->id("IBUF")].port_xform[ctx->id("O")] = ctx->id("OUT");
    hpiobuf_rules[ctx->id("IBUF_INTERMDISABLE")] = hpiobuf_rules[ctx->id("IBUF")];
    hpiobuf_rules[ctx->id("IBUF_IBUFDISABLE")] = hpiobuf_rules[ctx->id("IBUF")];
    hriobuf_rules[ctx->id("IBUFDS_INTERMDISABLE_INT")] = hriobuf_rules[ctx->id("IBUF")];
    hpiobuf_rules[ctx->id("IBUFDS_INTERMDISABLE_INT")].port_xform[ctx->id("IB")] = ctx->id("DIFFI_IN");
    hpiobuf_rules[ctx->id("IBUFDS")] = hpiobuf_rules[ctx->id("IBUF")];
    hpiobuf_rules[ctx->id("IBUFDS")].port_xform[ctx->id("IB")] = ctx->id("DIFFI_IN");

    // Special xform for OBUFx and IBUFx.
    std::unordered_map<IdString, XFormRule> rules;
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (!ci->attrs.count(ctx->id("BEL")))
            continue;
        std::string belname = ci->attrs[ctx->id("BEL")].c_str();
        size_t pos = belname.find("/");
        if (belname.substr(pos+1, 5) == "IOB18")
            rules = hpiobuf_rules;
        else if (belname.substr(pos+1, 5) == "IOB33")
            rules = hriobuf_rules;
        else
            log_error("Unexpected IOBUF BEL %s\n", belname.c_str());
        if (rules.count(ci->type)) {
            xform_cell(rules, ci);
        }
    }

    std::unordered_map<IdString, XFormRule> hrio_rules;
    hrio_rules[ctx->id("PAD")].new_type = ctx->id("PAD");

    hrio_rules[ctx->id("INV")].new_type = ctx->id("INVERTER");
    hrio_rules[ctx->id("INV")].port_xform[ctx->id("I")] = ctx->id("IN");
    hrio_rules[ctx->id("INV")].port_xform[ctx->id("O")] = ctx->id("OUT");

    hrio_rules[ctx->id("PS7")].new_type = ctx->id("PS7_PS7");

    generic_xform(hrio_rules, true);

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        std::string type = ci->type.str(ctx);
        if (!boost::starts_with(type, "IOB33") && !boost::starts_with(type, "IOB18"))
            continue;
        if (!ci->attrs.count(ctx->id("X_IOB_SITE_TYPE")))
            continue;
        type.replace(0, 5, ci->attrs.at(ctx->id("X_IOB_SITE_TYPE")).as_string());
        ci->type = ctx->id(type);
    }
}

std::string XC7Packer::get_ologic_site(const std::string &io_bel)
{
    BelId ibc_bel;
    if (io_bel.find("IOB18") != std::string::npos)
        ibc_bel = ctx->getBelByName(ctx->id(io_bel.substr(0, io_bel.find('/')) + "/IOB18/OUTBUF_DCIEN"));
    else
        ibc_bel = ctx->getBelByName(ctx->id(io_bel.substr(0, io_bel.find('/')) + "/IOB33/OUTBUF"));
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
    BelId ibc_bel;
    if (io_bel.find("IOB18") != std::string::npos)
        ibc_bel = ctx->getBelByName(ctx->id(io_bel.substr(0, io_bel.find('/')) + "/IOB18/INBUF_DCIEN"));
    else
      ibc_bel = ctx->getBelByName(ctx->id(io_bel.substr(0, io_bel.find('/')) + "/IOB33/INBUF_EN"));
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

std::string XC7Packer::get_idelay_site(const std::string &io_bel)
{
    BelId ibc_bel;
    if (io_bel.find("IOB18") != std::string::npos)
        ibc_bel = ctx->getBelByName(ctx->id(io_bel.substr(0, io_bel.find('/')) + "/IOB18/INBUF_DCIEN"));
    else
      ibc_bel = ctx->getBelByName(ctx->id(io_bel.substr(0, io_bel.find('/')) + "/IOB33/INBUF_EN"));
    std::queue<WireId> visit;
    visit.push(ctx->getBelPinWire(ibc_bel, ctx->id("OUT")));

    while (!visit.empty()) {
        WireId cursor = visit.front();
        visit.pop();
        for (auto bp : ctx->getWireBelPins(cursor)) {
            std::string site = ctx->getBelSite(bp.bel);
            if (boost::starts_with(site, "IDELAY"))
                return site;
        }
        for (auto pip : ctx->getPipsDownhill(cursor))
            visit.push(ctx->getPipDstWire(pip));
    }
    NPNR_ASSERT_FALSE("failed to find IDELAY");
}

std::string XC7Packer::get_ioctrl_site(const std::string &io_bel)
{
    std::vector<std::string> parts;
    boost::split(parts, io_bel, boost::is_any_of("/"));
    auto loc         = parts[0];
    auto iobank      = parts[1];
    auto pad_bel_str = loc + "/" + iobank + "/PAD";
    auto msg         = "could not get bel for: '" + pad_bel_str + "'";

    BelId pad_bel = ctx->getBelByName(ctx->id(pad_bel_str));
    NPNR_ASSERT_MSG(0 <= pad_bel.tile && 0 <= pad_bel.index, msg.c_str());

    int hclk_tile = ctx->getHclkForIob(pad_bel);
    auto &td = ctx->chip_info->tile_insts[hclk_tile];
    for (int i = 0; i < td.num_sites; i++) {
        auto &sd = td.site_insts[i];
        std::string sn = sd.name.get();
        if (boost::starts_with(sn, "IDELAYCTRL"))
            return sn;
    }
    NPNR_ASSERT_FALSE("failed to find IOCTRL");
}

void XC7Packer::fold_inverter(CellInfo *cell, std::string port)
{
    IdString p = ctx->id(port);
    NetInfo *net = get_net_or_empty(cell, p);
    if (net == nullptr)
        return;
    CellInfo *drv = net->driver.cell;
    if (drv == nullptr)
        return;
    if (drv->type == ctx->id("LUT1") && int_or_default(drv->params, ctx->id("INIT"), 0) == 1) {
        disconnect_port(ctx, cell, p);
        NetInfo *preinv = get_net_or_empty(drv, ctx->id("I0"));
        connect_port(ctx, preinv, cell, p);
        cell->params[ctx->id("IS_" + port + "_INVERTED")] = 1;
        if (net->users.empty())
            packed_cells.insert(drv->name);
    } else if (drv->type == ctx->id("INV")) {
        disconnect_port(ctx, cell, p);
        NetInfo *preinv = get_net_or_empty(drv, ctx->id("I"));
        connect_port(ctx, preinv, cell, p);
        cell->params[ctx->id("IS_" + port + "_INVERTED")] = 1;
        if (net->users.empty())
            packed_cells.insert(drv->name);
    }
}

void XC7Packer::pack_iologic()
{
    std::unordered_map<IdString, BelId> iodelay_to_io;
    std::unordered_map<IdString, XFormRule> iologic_rules;
    iologic_rules[ctx->id("OSERDESE2")].new_type = ctx->id("OSERDESE2_OSERDESE2");
    iologic_rules[ctx->id("IDELAYE2")].new_type = ctx->id("IDELAYE2_IDELAYE2");
    iologic_rules[ctx->id("ISERDESE2")].new_type = ctx->id("ISERDESE2_ISERDESE2");

    // Handles pseudo-diff output buffers without finding multiple sinks
    auto find_p_outbuf = [&](NetInfo *net) {
        CellInfo *outbuf = nullptr;
        for (auto &usr : net->users) {
            IdString type = usr.cell->type;
            if (type == ctx->id("IOB33_OUTBUF") || type == ctx->id("IOB33M_OUTBUF")
                || type == ctx->id("IOB18_OUTBUF_DCIEN") || type == ctx->id("IOB18M_OUTBUF_DCIEN")) {
                if (outbuf != nullptr)
                    return (CellInfo *)nullptr; // drives multiple outputs
                outbuf = usr.cell;
            }
        }
        return outbuf;
    };

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("IDELAYE2")) {
            NetInfo *d = get_net_or_empty(ci, ctx->id("IDATAIN"));
            if (d == nullptr || d->driver.cell == nullptr)
                log_error("%s '%s' has disconnected IDATAIN input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            CellInfo *drv = d->driver.cell;
            BelId io_bel;
            if (drv->type.str(ctx).find("INBUF_EN") != std::string::npos
                || drv->type.str(ctx).find("INBUF_DCIEN") != std::string::npos)
                io_bel = ctx->getBelByName(ctx->id(drv->attrs.at(ctx->id("BEL")).as_string()));
            else
                log_error("%s '%s' has IDATAIN input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                          ctx->nameOf(ci), drv->type.c_str(ctx));
            std::string iol_site = get_idelay_site(ctx->getBelName(io_bel).str(ctx));
            ci->attrs[ctx->id("BEL")] = iol_site + "/IDELAYE2";
            ci->attrs[ctx->id("X_IO_BEL")] = ctx->getBelName(io_bel).str(ctx);
            iodelay_to_io[ci->name] = io_bel;
        }
        // FIXME: ODELAY
    }

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("OSERDESE2")) {
            NetInfo *q = get_net_or_empty(ci, ctx->id("OQ"));
            if (q == nullptr || q->users.empty())
                log_error("%s '%s' has disconnected OQ output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            BelId io_bel;
            CellInfo *ob = find_p_outbuf(q);
            if (ob != nullptr)
                io_bel = ctx->getBelByName(ctx->id(ob->attrs.at(ctx->id("BEL")).as_string()));
            else
                log_error("%s '%s' has illegal fanout on OQ output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            std::string ol_site = get_ologic_site(ctx->getBelName(io_bel).str(ctx));
            ci->attrs[ctx->id("BEL")] = ol_site + "/OSERDESE2";
        } else if (ci->type == ctx->id("ISERDESE2")) {
            fold_inverter(ci, "CLKB");
            fold_inverter(ci, "OCLKB");

            std::string iobdelay = str_or_default(ci->params, ctx->id("IOBDELAY"), "NONE");
            BelId io_bel;

            if (iobdelay == "IFD") {
                NetInfo *d = get_net_or_empty(ci, ctx->id("DDLY"));
                if (d == nullptr || d->driver.cell == nullptr)
                    log_error("%s '%s' has disconnected DDLY input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
                CellInfo *drv = d->driver.cell;
                if (drv->type.str(ctx).find("IDELAYE2") != std::string::npos && d->driver.port == ctx->id("DATAOUT"))
                    io_bel = iodelay_to_io.at(drv->name);
                else
                    log_error("%s '%s' has DDLY input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                              ctx->nameOf(ci), drv->type.c_str(ctx));
            } else if (iobdelay == "NONE") {
                NetInfo *d = get_net_or_empty(ci, ctx->id("D"));
                if (d == nullptr || d->driver.cell == nullptr)
                    log_error("%s '%s' has disconnected D input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
                CellInfo *drv = d->driver.cell;
                if (drv->type.str(ctx).find("INBUF_EN") != std::string::npos
                    || drv->type.str(ctx).find("INBUF_DCIEN") != std::string::npos)
                    io_bel = ctx->getBelByName(ctx->id(drv->attrs.at(ctx->id("BEL")).as_string()));
                else
                    log_error("%s '%s' has D input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                              ctx->nameOf(ci), drv->type.c_str(ctx));
            } else {
                log_error("%s '%s' has unsupported IOBDELAY value '%s'\n", ci->type.c_str(ctx), ctx->nameOf(ci),
                          iobdelay.c_str());
            }

            std::string iol_site = get_ilogic_site(ctx->getBelName(io_bel).str(ctx));
            ci->attrs[ctx->id("BEL")] = iol_site + "/ISERDESE2";
        }
    }

    flush_cells();
    generic_xform(iologic_rules, false);
    flush_cells();
}

void XC7Packer::pack_idelayctrl()
{
    CellInfo *idelayctrl = nullptr;
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("IDELAYCTRL")) {
            if (idelayctrl != nullptr)
                log_error("Found more than one IDELAYCTRL cell!\n");
            idelayctrl = ci;
        }
    }
    if (idelayctrl == nullptr)
        return;
    std::set<std::string> ioctrl_sites;
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("IDELAYE2_IDELAYE2") || ci->type == ctx->id("ODELAYE2_ODELAYE2")) {
            if (!ci->attrs.count(ctx->id("BEL")))
                continue;
            ioctrl_sites.insert(get_ioctrl_site(ci->attrs.at(ctx->id("X_IO_BEL")).as_string()));
        }
    }
    if (ioctrl_sites.empty())
        log_error("Found IDELAYCTRL but no I/ODELAYs\n");
    NetInfo *rdy = get_net_or_empty(idelayctrl, ctx->id("RDY"));
    disconnect_port(ctx, idelayctrl, ctx->id("RDY"));
    std::vector<NetInfo *> dup_rdys;
    int i = 0;
    for (auto site : ioctrl_sites) {
        auto dup_idc = create_cell(ctx, ctx->id("IDELAYCTRL"),
                                   int_name(idelayctrl->name, "CTRL_DUP_" + std::to_string(i), false));
        connect_port(ctx, get_net_or_empty(idelayctrl, ctx->id("REFCLK")), dup_idc.get(), ctx->id("REFCLK"));
        connect_port(ctx, get_net_or_empty(idelayctrl, ctx->id("RST")), dup_idc.get(), ctx->id("RST"));
        if (rdy != nullptr) {
            NetInfo *dup_rdy =
                    (ioctrl_sites.size() == 1)
                            ? rdy
                            : create_internal_net(idelayctrl->name, "CTRL_DUP_" + std::to_string(i) + "_RDY", false);
            connect_port(ctx, dup_rdy, dup_idc.get(), ctx->id("RDY"));
            dup_rdys.push_back(dup_rdy);
        }
        dup_idc->attrs[ctx->id("BEL")] = site + "/IDELAYCTRL";
        new_cells.push_back(std::move(dup_idc));
        ++i;
    }
    disconnect_port(ctx, idelayctrl, ctx->id("REFCLK"));
    disconnect_port(ctx, idelayctrl, ctx->id("RST"));

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

    ioctrl_rules[ctx->id("IDELAYCTRL")].new_type = ctx->id("IDELAYCTRL_IDELAYCTRL");

    generic_xform(ioctrl_rules);
}

NEXTPNR_NAMESPACE_END
