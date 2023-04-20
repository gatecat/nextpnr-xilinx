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
    pool<WireId> visited;
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
    if (cell->attrs.count(id_BEL))
        return;
    NetInfo *n = cell->getPort(port);
    if (n == nullptr || n->driver.cell == nullptr)
        return;
    CellInfo *drv = n->driver.cell;
    if (!drv->attrs.count(id_BEL))
        return;
    BelId drv_bel = ctx->getBelByNameStr(drv->attrs.at(id_BEL).as_string());
    if (drv_bel == BelId())
        return;
    WireId drv_wire = ctx->getBelPinWire(drv_bel, n->driver.port);
    if (drv_wire == WireId())
        return;
    BelId tgt = find_bel_with_short_route(drv_wire, cell->type, port);
    if (tgt != BelId()) {
        used_bels.insert(tgt);
        cell->attrs[id_BEL] = std::string(ctx->nameOfBel(tgt));
        log_info("    Constrained %s '%s' to bel '%s' based on dedicated routing\n", cell->type.c_str(ctx),
                 ctx->nameOf(cell), ctx->nameOfBel(tgt));
    }
}

void XilinxPacker::preplace_unique(CellInfo *cell)
{
    if (cell->attrs.count(id_BEL))
        return;
    for (auto bel : ctx->getBels()) {
        if (ctx->checkBelAvail(bel) && ctx->getBelType(bel) == cell->type && !used_bels.count(bel)) {
            cell->attrs[id_BEL] = std::string(ctx->nameOfBel(bel));
            used_bels.insert(bel);
            return;
        }
    }
}

void USPacker::prepare_clocking()
{
    log_info("Preparing clocking...\n");
    dict<IdString, IdString> upgrade;
    upgrade[id_MMCME2_ADV] = id_MMCME4_ADV;
    upgrade[id_MMCME4_BASIC] = id_MMCME4_ADV;
    upgrade[id_PLLE4_BASIC] = id_PLLE4_ADV;
    upgrade[id_BUFG] = id_BUFGCE;

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (upgrade.count(ci->type)) {
            IdString new_type = upgrade.at(ci->type);
            ci->type = new_type;
        }
        if (ci->attrs.count(id_BEL))
            used_bels.insert(ctx->getBelByNameStr(ci->attrs.at(id_BEL).as_string()));
    }
}

void USPacker::pack_plls()
{
    log_info("Packing PLLs...\n");

    auto set_default = [](CellInfo *ci, IdString param, const Property &value) {
        if (!ci->params.count(param))
            ci->params[param] = value;
    };

    dict<IdString, XFormRule> pll_rules;
    pll_rules[id_MMCME4_ADV].new_type = id_MMCM_MMCM_TOP;
    pll_rules[id_PLLE4_ADV].new_type = id_PLL_PLL_TOP;
    generic_xform(pll_rules);
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        // Preplace PLLs to make use of dedicated/short routing paths
        if (ci->type.in(id_MMCM_MMCM_TOP, id_PLL_PLL_TOP))
            try_preplace(ci, id_CLKIN1);
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
            set_default(ci, id_COMPENSATION, Property("INTERNAL"));

            // Fixup routing
            if (str_or_default(ci->params, id_COMPENSATION, "INTERNAL") == "INTERNAL") {
                ci->disconnectPort(id_CLKFBIN);
                ci->connectPort(id_CLKFBIN, ctx->nets[ctx->id("$PACKER_VCC_NET")].get());
            }
        }
    }
}

void USPacker::pack_gbs()
{
    log_info("Packing global buffers...\n");
    dict<IdString, XFormRule> gb_rules;
    gb_rules[id_BUFGCTRL].new_type = id_BUFGCTRL;
    gb_rules[id_BUFG_PS].new_type = id_BUFCE_BUFG_PS;
    gb_rules[id_BUFGCE_DIV].new_type = id_BUFGCE_DIV_BUFGCE_DIV;
    gb_rules[id_BUFGCE].new_type = id_BUFCE_BUFCE;

    // Make sure prerequisites are set up first
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_PSS_ALTO_CORE)
            preplace_unique(ci);
    }

    generic_xform(gb_rules);

    // Preplace global buffers to make use of dedicated/short routing
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_BUFGCTRL)
            try_preplace(ci, id_I0);
        if (ci->type.in(id_BUFCE_BUFG_PS, id_BUFGCE_DIV_BUFGCE_DIV, id_BUFCE_BUFCE))
            try_preplace(ci, id_I);
    }
}

void USPacker::pack_clocking()
{
    pack_plls();
    pack_gbs();
}

NEXTPNR_NAMESPACE_END