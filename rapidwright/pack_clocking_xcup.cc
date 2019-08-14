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

BelId XilinxPacker::find_bel_with_short_route(WireId source, IdString beltype, IdString belpin)
{
    if (source == WireId())
        return BelId();
    const size_t max_visit = 50000; // effort/runtime tradeoff
    std::unordered_set<WireId> visited;
    std::queue<WireId> visit;
    visit.push(source);
    while (!visit.empty() && visited.size() < max_visit) {
        WireId cursor = visit.front();
        visit.pop();
        for (auto bp : ctx->getWireBelPins(cursor))
            if (bp.pin == belpin && ctx->getBelType(bp.bel) == beltype && !used_bels.count(bp.bel))
                return bp.bel;
        for (auto pip : ctx->getPipsDownhill(cursor)) {
            WireId dst = ctx->getPipDstWire(pip);
            if (visited.count(dst))
                continue;
            visit.push(dst);
            visited.insert(dst);
        }
    }
    return BelId();
}

void XilinxPacker::try_preplace(CellInfo *cell, IdString port)
{
    if (cell->attrs.count(ctx->id("BEL")))
        return;
    NetInfo *n = get_net_or_empty(cell, port);
    if (n == nullptr || n->driver.cell == nullptr)
        return;
    CellInfo *drv = n->driver.cell;
    if (!drv->attrs.count(ctx->id("BEL")))
        return;
    BelId drv_bel = ctx->getBelByName(ctx->id(drv->attrs.at(ctx->id("BEL")).as_string()));
    if (drv_bel == BelId())
        return;
    WireId drv_wire = ctx->getBelPinWire(drv_bel, n->driver.port);
    if (drv_wire == WireId())
        return;
    BelId tgt = find_bel_with_short_route(drv_wire, cell->type, port);
    if (tgt != BelId()) {
        used_bels.insert(tgt);
        cell->attrs[ctx->id("BEL")] = std::string(ctx->nameOfBel(tgt));
        log_info("    Constrained %s '%s' to bel '%s' based on dedicated routing\n", cell->type.c_str(ctx),
                 ctx->nameOf(cell), ctx->nameOfBel(tgt));
    }
}

void XilinxPacker::preplace_unique(CellInfo *cell)
{
    if (cell->attrs.count(ctx->id("BEL")))
        return;
    for (auto bel : ctx->getBels()) {
        if (ctx->checkBelAvail(bel) && ctx->getBelType(bel) == cell->type && !used_bels.count(bel)) {
            cell->attrs[ctx->id("BEL")] = std::string(ctx->nameOfBel(bel));
            used_bels.insert(bel);
            return;
        }
    }
}

void USPacker::prepare_clocking()
{
    log_info("Preparing clocking...\n");
    std::unordered_map<IdString, IdString> upgrade;
    upgrade[ctx->id("MMCME2_ADV")] = ctx->id("MMCME4_ADV");
    upgrade[ctx->id("MMCME4_BASIC")] = ctx->id("MMCME4_ADV");
    upgrade[ctx->id("PLLE4_BASIC")] = ctx->id("PLLE4_ADV");
    upgrade[ctx->id("BUFG")] = ctx->id("BUFGCE");

    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (upgrade.count(ci->type)) {
            IdString new_type = upgrade.at(ci->type);
            ci->type = new_type;
        }
        if (ci->attrs.count(ctx->id("BEL")))
            used_bels.insert(ctx->getBelByName(ctx->id(ci->attrs.at(ctx->id("BEL")).as_string())));
    }
}

void USPacker::pack_plls()
{
    log_info("Packing PLLs...\n");

    auto set_default = [](CellInfo *ci, IdString param, const Property &value) {
        if (!ci->params.count(param))
            ci->params[param] = value;
    };

    std::unordered_map<IdString, XFormRule> pll_rules;
    pll_rules[ctx->id("MMCME4_ADV")].new_type = id_MMCM_MMCM_TOP;
    pll_rules[ctx->id("PLLE4_ADV")].new_type = id_PLL_PLL_TOP;
    generic_xform(pll_rules);
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        // Preplace PLLs to make use of dedicated/short routing paths
        if (ci->type == id_MMCM_MMCM_TOP || ci->type == id_PLL_PLL_TOP)
            try_preplace(ci, ctx->id("CLKIN1"));
        if (ci->type == id_MMCM_MMCM_TOP) {
            // Fixup parameters
            for (int i = 1; i <= 2; i++)
                set_default(ci, ctx->id("CLKIN" + std::to_string(i) + "_PERIOD"), Property("0.0"));
            for (int i = 0; i <= 6; i++) {
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_CASCADE"), Property("FALSE"));
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_DIVIDE"), Property(1));
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_DUTY_CYCLE"), Property("0.5"));
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_PHASE"), Property(0));
                set_default(ci, ctx->id("CLKOUT" + std::to_string(i) + "_USE_FINE_PS"), Property("FALSE"));
            }
            set_default(ci, ctx->id("COMPENSATION"), Property("INTERNAL"));

            // Fixup routing
            if (str_or_default(ci->params, ctx->id("COMPENSATION"), "INTERNAL") == "INTERNAL") {
                disconnect_port(ctx, ci, ctx->id("CLKFBIN"));
                connect_port(ctx, ctx->nets[ctx->id("$PACKER_VCC_NET")].get(), ci, ctx->id("CLKFBIN"));
            }
        }
    }
}

void USPacker::pack_gbs()
{
    log_info("Packing global buffers...\n");
    std::unordered_map<IdString, XFormRule> gb_rules;
    gb_rules[id_BUFGCTRL].new_type = id_BUFGCTRL;
    gb_rules[ctx->id("BUFG_PS")].new_type = id_BUFCE_BUFG_PS;
    gb_rules[ctx->id("BUFGCE_DIV")].new_type = id_BUFGCE_DIV_BUFGCE_DIV;
    gb_rules[ctx->id("BUFGCE")].new_type = id_BUFCE_BUFCE;

    // Make sure prerequisites are set up first
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == ctx->id("PSS_ALTO_CORE"))
            preplace_unique(ci);
    }

    generic_xform(gb_rules);

    // Preplace global buffers to make use of dedicated/short routing
    for (auto cell : sorted(ctx->cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id_BUFGCTRL)
            try_preplace(ci, ctx->id("I0"));
        if (ci->type == id_BUFCE_BUFG_PS || ci->type == id_BUFGCE_DIV_BUFGCE_DIV || ci->type == id_BUFCE_BUFCE)
            try_preplace(ci, ctx->id("I"));
    }
}

void USPacker::pack_clocking()
{
    pack_plls();
    pack_gbs();
}

NEXTPNR_NAMESPACE_END