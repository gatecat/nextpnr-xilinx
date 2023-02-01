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

bool USPacker::has_illegal_fanout(NetInfo *carry)
{
    // FIXME: sometimes we can feed out of the chain
    if (carry->users.entries() > 2)
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
        NetInfo *muxcy_s = muxcy->getPort(ctx->id("S"));
        NetInfo *xorcy_li = xorcy->getPort(ctx->id("LI"));
        if (muxcy_s != xorcy_li)
            return true;
    }
    return false;
}

void USPacker::pack_carries()
{
    log_info("Packing carries..\n");
    split_carry4s();
    std::vector<CellInfo *> root_muxcys;
    // Find MUXCYs
    for (auto& cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type != ctx->id("MUXCY"))
            continue;
        NetInfo *ci_net = ci->getPort(ctx->id("CI"));
        if (ci_net == nullptr || ci_net->driver.cell == nullptr || ci_net->driver.cell->type != ctx->id("MUXCY") ||
            has_illegal_fanout(ci_net)) {
            root_muxcys.push_back(ci);
        }
    }

    // Create chains from root MUXCYs
    pool<IdString> processed_muxcys;
    std::vector<CarryGroup> groups;
    int muxcy_count = 0, xorcy_count = 0;
    for (auto root : root_muxcys) {
        CarryGroup group;

        CellInfo *muxcy = root;
        NetInfo *mux_ci = nullptr;
        while (true) {

            group.muxcys.push_back(muxcy);
            ++muxcy_count;
            mux_ci = muxcy->getPort(ctx->id("CI"));
            NetInfo *mux_s = muxcy->getPort(ctx->id("S"));
            group.xorcys.push_back(nullptr);
            if (mux_s != nullptr) {
                for (auto &user : mux_s->users) {
                    if (user.cell->type == ctx->id("XORCY") && user.port == ctx->id("LI")) {
                        CellInfo *xorcy = user.cell;
                        NetInfo *xor_ci = xorcy->getPort(ctx->id("CI"));
                        if (xor_ci == mux_ci) {
                            group.xorcys.back() = xorcy;
                            ++xorcy_count;
                            break;
                        }
                    }
                }
            }

            mux_ci = muxcy->getPort(ctx->id("O"));
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
            if (mux_ci->users.entries() == 1 && (*mux_ci->users.begin()).cell->type == ctx->id("XORCY") &&
                (*mux_ci->users.begin()).port == ctx->id("CI")) {
                // Trailing XORCY at end, can pack into chain.
                CellInfo *xorcy = (*mux_ci->users.begin()).cell;
                std::unique_ptr<CellInfo> dummy_muxcy =
                        create_cell(ctx, ctx->id("MUXCY"), ctx->id(xorcy->name.str(ctx) + "$legal_muxcy$"));
                dummy_muxcy->connectPort(ctx->id("CI"), mux_ci);
                dummy_muxcy->connectPort(ctx->id("S"), xorcy->getPort(ctx->id("LI")));
                group.muxcys.push_back(dummy_muxcy.get());
                group.xorcys.push_back(xorcy);
                new_cells.push_back(std::move(dummy_muxcy));
            } else if (mux_ci->users.entries() > 0) {
                // Users other than a MUXCY
                // Feed out with a zero-driving LUT and a XORCY
                // (creating a zero-driver using Vcc and an inverter for now...)
                std::unique_ptr<CellInfo> zero_lut =
                        create_lut(ctx, mux_ci->name.str(ctx) + "$feed$zero",
                                   {ctx->nets[ctx->id("$PACKER_VCC_NET")].get()}, nullptr, Property(1));
                std::unique_ptr<CellInfo> feed_xorcy =
                        create_cell(ctx, ctx->id("XORCY"), ctx->id(mux_ci->name.str(ctx) + "$feed$xor"));
                std::unique_ptr<CellInfo> dummy_muxcy =
                        create_cell(ctx, ctx->id("MUXCY"), ctx->id(mux_ci->name.str(ctx) + "$feed$muxcy"));

                CellInfo *last_muxcy = mux_ci->driver.cell;

                last_muxcy->disconnectPort(ctx->id("O"));

                zero_lut->connectPorts(ctx->id("O"), feed_xorcy.get(), ctx->id("LI"));
                zero_lut->connectPorts(ctx->id("O"), dummy_muxcy.get(), ctx->id("S"));
                last_muxcy->connectPorts(ctx->id("O"), feed_xorcy.get(), ctx->id("CI"));
                last_muxcy->connectPorts(ctx->id("O"), dummy_muxcy.get(), ctx->id("CI"));

                feed_xorcy->connectPort(ctx->id("O"), mux_ci);

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

    log_info("   Grouped %d MUXCYs and %d XORCYs into %d chains.\n", muxcy_count, xorcy_count, int(root_muxcys.size()));

    // N.B. LUT6 is not a valid type here, as CARRY requires dual outputs
    pool<IdString> lut_types{ctx->id("LUT1"), ctx->id("LUT2"), ctx->id("LUT3"), ctx->id("LUT4"),
                                           ctx->id("LUT5")};

    pool<IdString> folded_nets;

    for (auto &grp : groups) {
        std::vector<std::unique_ptr<CellInfo>> carry8s;
        for (int i = 0; i < int(grp.muxcys.size()); i++) {
            int z = i % 8;
            CellInfo *muxcy = grp.muxcys.at(i), *xorcy = grp.xorcys.at(i);
            if (z == 0)
                carry8s.push_back(
                        create_cell(ctx, ctx->id("CARRY8"), ctx->id(muxcy->name.str(ctx) + "$PACKED_CARRY8$")));
            CellInfo *c8 = carry8s.back().get();
            CellInfo *root = carry8s.front().get();
            if (i == 0) {
                // Constrain initial CARRY8, forcing it to the CARRY8 of a logic tile
                c8->cluster = c8->name;
                c8->constr_abs_z = true;
                c8->constr_z = BEL_CARRY8;
            } else if (z == 0) {
                // Constrain relative to the root carry8
                c8->cluster = root->name;
                root->constr_children.push_back(c8);
                c8->constr_x = 0;
                c8->constr_y = -i / 8;
                c8->constr_abs_z = true;
                c8->constr_z = BEL_CARRY8;
            }
            // Fold CI->CO connections into the CARRY8, except for those external ones every 8 units
            if (z == 0) {
                muxcy->movePortTo(ctx->id("CI"), c8, ctx->id("CI"));
            } else {
                NetInfo *muxcy_ci = muxcy->getPort(ctx->id("CI"));
                if (muxcy_ci)
                    folded_nets.insert(muxcy_ci->name);
                muxcy->disconnectPort(ctx->id("CI"));
            }
            if (z == 7) {
                muxcy->movePortTo(ctx->id("O"), c8, ctx->id("CO[7]"));
            } else {
                NetInfo *muxcy_o = muxcy->getPort(ctx->id("O"));
                if (muxcy_o)
                    folded_nets.insert(muxcy_o->name);
                muxcy->disconnectPort(ctx->id("O"));
            }
            // Replace connections into the MUXCY with external CARRY8 ports
            muxcy->movePortTo(ctx->id("S"), c8, ctx->id("S[" + std::to_string(z) + "]"));
            muxcy->movePortTo(ctx->id("DI"), c8, ctx->id("DI[" + std::to_string(z) + "]"));
            packed_cells.insert(muxcy->name);
            // Fold MUXCY->XORCY into the CARRY8, if there is a XORCY
            if (xorcy) {
                // Replace XORCY output with external CARRY8 output
                xorcy->movePortTo(ctx->id("O"), c8, ctx->id("O[" + std::to_string(z) + "]"));
                // Disconnect internal XORCY connectivity
                xorcy->disconnectPort(ctx->id("LI"));
                xorcy->disconnectPort(ctx->id("DI"));
                packed_cells.insert(xorcy->name);
            }
            // Check legality of LUTs driving CARRY8, making them legal if they aren't already
            NetInfo *c8_s = c8->getPort(ctx->id("S[" + std::to_string(z) + "]"));
            NetInfo *c8_di = c8->getPort(ctx->id("DI[" + std::to_string(z) + "]"));
            // Keep track of the total LUT input count; cannot exceed five or the LUTs cannot be packed together
            pool<IdString> unique_lut_inputs;
            int s_inputs = 0, d_inputs = 0;
            // Check that S and DI are validy and unqiuely driven by LUTs
            // FIXME: in multiple fanout cases, cell duplication will probably be cheaper
            // than feed-throughs
            CellInfo *s_lut = nullptr, *di_lut = nullptr;
            if (c8_s) {
                if (c8_s->users.entries() == 1 && c8_s->driver.cell != nullptr &&
                    lut_types.count(c8_s->driver.cell->type)) {
                    s_lut = c8_s->driver.cell;
                    for (int j = 0; j < 5; j++) {
                        NetInfo *ix = s_lut->getPort(ctx->id("I" + std::to_string(j)));
                        if (ix) {
                            unique_lut_inputs.insert(ix->name);
                            s_inputs++;
                        }
                    }
                }
            }
            if (c8_di) {
                if (c8_di->users.entries() == 1 && c8_di->driver.cell != nullptr &&
                    lut_types.count(c8_di->driver.cell->type)) {
                    di_lut = c8_di->driver.cell;
                    for (int j = 0; j < 5; j++) {
                        NetInfo *ix = di_lut->getPort(ctx->id("I" + std::to_string(j)));
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
                s_lut->cluster = root->name;
                s_lut->constr_x = 0;
                s_lut->constr_y = -i / 8;
                s_lut->constr_abs_z = true;
                s_lut->constr_z = (z << 4 | BEL_6LUT);
            }
            if (di_lut) {
                root->constr_children.push_back(di_lut);
                di_lut->cluster = root->name;
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
    dict<IdString, XFormRule> softlogic_rules;
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
    dict<IdString, XFormRule> c8_chained_rules;
    c8_chained_rules[ctx->id("CARRY8")].new_type = ctx->id("CARRY8");
    c8_chained_rules[ctx->id("CARRY8")].port_xform[ctx->id("CI")] = ctx->id("CIN");
    c8_chained_rules[ctx->id("CARRY8")].set_params.emplace_back(ctx->id("CARRY_TYPE"), Property("SINGLE_CY8"));
    dict<IdString, XFormRule> c8_init_rules;
    c8_init_rules[ctx->id("CARRY8")].new_type = ctx->id("CARRY8");
    c8_init_rules[ctx->id("CARRY8")].port_xform[ctx->id("CI")] = ctx->id("AX");
    c8_init_rules[ctx->id("CARRY8")].set_params.emplace_back(ctx->id("CARRY_TYPE"), Property("SINGLE_CY8"));

    for (auto& cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type != ctx->id("CARRY8"))
            continue;
        if (ci->cluster == ClusterId())
            xform_cell(c8_init_rules, ci);
        else
            xform_cell(c8_chained_rules, ci);
        ci->ports[ctx->id("EX")].name = ctx->id("EX");
        ci->ports[ctx->id("EX")].type = PORT_IN;
        ci->connectPort(ctx->id("EX"), ctx->nets[ctx->id("$PACKER_GND_NET")].get());
    }
}

NEXTPNR_NAMESPACE_END