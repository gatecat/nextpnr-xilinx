/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  David Shah <dave@ds0.me>
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
#include "cells.h"
#include "pack.h"

NEXTPNR_NAMESPACE_BEGIN

void USPacker::pack_dsps()
{
    const std::vector<IdString> dsp_subcell_names = {
            id_DSP_PREADD_DATA, id_DSP_PREADD, id_DSP_A_B_DATA, id_DSP_MULTIPLIER,
            id_DSP_C_DATA,      id_DSP_M_DATA, id_DSP_ALU,      id_DSP_OUTPUT};

    for (auto& cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type != id_DSP48E2)
            continue;

        // First of all, trim pins that are connected to "ground" in synthesis but really should be floating if
        // don't care
        for (auto port : ci->ports) {
            std::string name = port.first.str(ctx);
            if (boost::starts_with(name, "ACIN") || boost::starts_with(name, "BCIN") ||
                boost::starts_with(name, "PCIN")) {
                NetInfo *pn = port.second.net;
                if (pn->name == ctx->id("$PACKER_GND_NET"))
                    ci->disconnectPort(port.first);
            }
        }

        /*
         * DSP primitives in UltraScale+ (but not xc7) are complex "macros" that expand to more than one BEL
         * we must track the mapping here for useful import on the other side; and preservation of parameters
         */
        dict<IdString, PortInfo> orig_ports = ci->ports;
        std::vector<CellInfo *> subcells;

        for (auto ctype : dsp_subcell_names) {
            std::unique_ptr<CellInfo> subcell =
                    create_dsp_cell(ctx, ctype, int_name(ci->name, ctype.str(ctx) + "_INST", true));

            if (!subcells.empty()) {
                // FIXME: add constraints for cascaded DSP chains
                subcell->cluster = subcells.front()->name;
                subcells.front()->constr_children.push_back(subcell.get());
                subcell->constr_x = 0;
                subcell->constr_y = 0;
                subcell->constr_z = int(subcells.size());
                subcell->constr_abs_z = false;
            } else {
                subcell->cluster = subcell->name;
            }

            CellInfo *sci = subcell.get();
            new_cells.push_back(std::move(subcell));
            subcells.push_back(sci);
        }

        for (int i = 0; i < 30; i++)
            ci->movePortTo(ctx->id("A[" + std::to_string(i) + "]"), subcells[BEL_DSP_A_B_DATA],
                         ctx->id("A[" + std::to_string(i) + "]"));
        for (int i = 0; i < 18; i++)
            ci->movePortTo(ctx->id("B[" + std::to_string(i) + "]"), subcells[BEL_DSP_A_B_DATA],
                         ctx->id("B[" + std::to_string(i) + "]"));
        for (int i = 0; i < 48; i++)
            ci->movePortTo(ctx->id("C[" + std::to_string(i) + "]"), subcells[BEL_DSP_C_DATA],
                         ctx->id("C[" + std::to_string(i) + "]"));
        for (int i = 0; i < 27; i++)
            ci->movePortTo(ctx->id("D[" + std::to_string(i) + "]"), subcells[BEL_DSP_PREADD_DATA],
                         ctx->id("DIN[" + std::to_string(i) + "]"));
        for (auto port : ci->ports) {
            IdString p = port.first;
            NetInfo *pn = port.second.net;
            if (pn == nullptr)
                continue;
            ci->disconnectPort(p);
            for (auto sc : subcells)
                if (sc->ports.count(p))
                    sc->connectPort(p, pn);
        }
        // Copy parameters from the original cell to all subcells
        for (auto sc : subcells)
            sc->params = ci->params;
        // Add ports of DSP "super-cell" as attributes
        for (auto sc : subcells) {
            sc->attrs[id_X_ORIG_MACRO_PRIM] = ci->type.str(ctx);
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

        packed_cells.insert(ci->name);
    }

    flush_cells();

    // Expand bus ports and set orig ports/type correctly
    dict<IdString, XFormRule> dsp_rules;
    for (auto sctype : dsp_subcell_names)
        dsp_rules[sctype].new_type = sctype;
    generic_xform(dsp_rules);
}

NEXTPNR_NAMESPACE_END