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

void XC7Packer::prepare_clocking()
{
    log_info("Preparing clocking...\n");
    dict<IdString, IdString> upgrade;
    upgrade[id_MMCME2_BASE] = id_MMCME2_ADV;
    upgrade[id_PLLE2_BASE] = id_PLLE2_ADV;

    for (auto& cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (upgrade.count(ci->type)) {
            IdString new_type = upgrade.at(ci->type);
            ci->type = new_type;
        } else if (ci->type == id_BUFG) {
            ci->type = id_BUFGCTRL;
            ci->renamePort(id_I, id_I0);
            tie_port(ci, "CE0", true, true);
            tie_port(ci, "S0", true, true);
            tie_port(ci, "S1", false, true);
            tie_port(ci, "IGNORE0", true, true);
        } else if (ci->type == id_BUFGCE) {
            ci->type = id_BUFGCTRL;
            ci->renamePort(id_I, id_I0);
            ci->renamePort(id_CE, id_CE0);
            tie_port(ci, "S0", true, true);
            tie_port(ci, "S1", false, true);
            tie_port(ci, "IGNORE0", true, true);
        }
        if (ci->attrs.count(id_BEL))
            used_bels.insert(ctx->getBelByNameStr(ci->attrs.at(id_BEL).as_string()));
    }
}

void XC7Packer::pack_plls()
{
    log_info("Packing PLLs...\n");

    auto set_default = [](CellInfo *ci, IdString param, const Property &value) {
        if (!ci->params.count(param))
            ci->params[param] = value;
    };

    dict<IdString, XFormRule> pll_rules;
    pll_rules[id_MMCME2_ADV].new_type = id_MMCME2_ADV_MMCME2_ADV;
    pll_rules[id_PLLE2_ADV].new_type = id_PLLE2_ADV_PLLE2_ADV;
    generic_xform(pll_rules);
    for (auto& cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        // Preplace PLLs to make use of dedicated/short routing paths
        if (ci->type == id_MMCM_MMCM_TOP || ci->type == id_PLL_PLL_TOP)
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

void XC7Packer::pack_gbs()
{
    log_info("Packing global buffers...\n");
    dict<IdString, XFormRule> gb_rules;
    gb_rules[id_BUFGCTRL].new_type = id_BUFGCTRL;
    gb_rules[id_BUFGCTRL].new_type = id_BUFGCTRL;

    generic_xform(gb_rules);

    // Make sure prerequisites are set up first
    for (auto& cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_PS7_PS7)
            preplace_unique(ci);
    }

    // Preplace global buffers to make use of dedicated/short routing
    for (auto& cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_BUFGCTRL)
            try_preplace(ci, id_I0);
        if (ci->type == id_BUFG_BUFG)
            try_preplace(ci, id_I);
    }
}

void XC7Packer::pack_clocking()
{
    pack_plls();
    pack_gbs();
}

NEXTPNR_NAMESPACE_END
