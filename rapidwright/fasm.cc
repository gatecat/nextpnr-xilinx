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

#include <boost/algorithm/string.hpp>
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
struct FasmBackend
{
    Context *ctx;
    std::ostream &out;

    std::unordered_map<std::string, std::pair<std::string, std::string>> used_sitepips;

    void write_pip(PipId pip, NetInfo *net){

    };

    // Get the set of input signals for a LUT-type cell
    std::vector<IdString> get_inputs(CellInfo *cell)
    {
        IdString type = ctx->id(str_or_default(cell->attrs, ctx->id("X_ORIG_TYPE"), ""));
        if (type == ctx->id("LUT1"))
            return {ctx->id("I0")};
        else if (type == ctx->id("LUT2"))
            return {ctx->id("I0"), ctx->id("I1")};
        else if (type == ctx->id("LUT3"))
            return {ctx->id("I0"), ctx->id("I1"), ctx->id("I2")};
        else if (type == ctx->id("LUT4"))
            return {ctx->id("I0"), ctx->id("I1"), ctx->id("I2"), ctx->id("I3")};
        else if (type == ctx->id("LUT5"))
            return {ctx->id("I0"), ctx->id("I1"), ctx->id("I2"), ctx->id("I3"), ctx->id("I4")};
        else if (type == ctx->id("LUT6"))
            return {ctx->id("I0"), ctx->id("I1"), ctx->id("I2"), ctx->id("I3"), ctx->id("I4"), ctx->id("I5")};
        else
            NPNR_ASSERT_FALSE("unsupported LUT-type cell");
    }

    // Process LUT initialisation
    std::vector<bool> get_lut_init(CellInfo *lut6, CellInfo *lut5)
    {
        std::vector<bool> bits(64, false);

        std::vector<IdString> phys_inputs;
        for (int i = 1; i <= 6; i++)
            phys_inputs.push_back(ctx->id("A" + std::to_string(i)));

        for (int i = 0; i < 2; i++) {
            CellInfo *lut = (i == 1) ? lut5 : lut6;
            if (lut == nullptr)
                continue;
            auto lut_inputs = get_inputs(lut);
            std::unordered_map<int, std::vector<std::string>> phys_to_log;
            std::unordered_map<std::string, int> log_to_bit;
            for (int j = 0; j < int(lut_inputs.size()); j++)
                log_to_bit[lut_inputs[j].str(ctx)] = j;
            for (int j = 0; j < 6; j++) {
                // Get the LUT physical to logical mapping
                phys_to_log[j];
                if (!lut->attrs.count(ctx->id("X_ORIG_PORT_" + phys_inputs[j].str(ctx))))
                    continue;
                std::string orig = lut->attrs.at(ctx->id("X_ORIG_PORT_" + phys_inputs[j].str(ctx))).as_string();
                boost::split(phys_to_log[j], orig, boost::is_any_of(" "));
            }
            int lbound = 0, ubound = 64;
            // Fracturable LUTs
            if (lut5 && lut6) {
                lbound = (i == 1) ? 0 : 32;
                ubound = (i == 1) ? 32 : 64;
            }
            Property init = get_or_default(lut->params, ctx->id("INIT"), Property()).extract(0, 64);
            for (int j = lbound; j < ubound; j++) {
                int log_index = 0;
                for (int k = 0; k < 6; k++) {
                    if ((j & (1 << k)) == 0)
                        continue;
                    for (auto &p2l : phys_to_log[k])
                        log_index |= (1 << log_to_bit[p2l]);
                }
                bits[j] = (init.str.at(log_index) == Property::S1);
            }
        }
        return bits;
    };

    // Return the name for a half-logic-tile
    std::string get_half_name(int half, bool is_m)
    {
        if (is_m)
            return half ? "SLICEL_X1" : "SLICEM_X0";
        else
            return half ? "SLICEL_X1" : "SLICEL_X0";
    }

    // Return the final part of a bel name
    std::string get_bel_name(BelId bel) { return IdString(ctx->locInfo(bel).bel_data[bel.index].name).str(ctx); }

    std::string get_tile_name(int tile) { return ctx->chip_info->tile_insts[tile].name.get(); }

    void write_routing_bel(const std::string &prefix, WireId dst_wire)
    {
        for (auto pip : ctx->getPipsUphill(dst_wire)) {
            if (ctx->getBoundPipNet(pip) != nullptr) {
                out << prefix;
                auto &pd = ctx->locInfo(pip).pip_data[pip.index];
                out << IdString(pd.bel).c_str(ctx) << "." << IdString(pd.extra_data).c_str(ctx);
                out << std::endl;
            }
        }
    }

    // Process flipflops in a half-tile
    void process_ffs(int tile, int half)
    {
        bool found_ff = false;
        bool is_latch = false;
        bool is_sync = false;
        bool is_clkinv = false;
        bool is_srused = false;
        bool is_ceused = false;
#define SET_CHECK(dst, src)                                                                                            \
    do {                                                                                                               \
        if (found_ff)                                                                                                  \
            NPNR_ASSERT(dst == (src));                                                                                 \
        else                                                                                                           \
            dst = (src);                                                                                               \
    } while (0)

        std::string tname = get_tile_name(tile);
        std::string hname = get_half_name(half, tname.find("CLBLM") != std::string::npos);

        auto lts = ctx->tileStatus[tile].lts;
        if (lts == nullptr)
            return;

        for (int i = 0; i < 4; i++) {
            CellInfo *ff1 = lts->cells[(half << 7) | (i << 4) | BEL_FF];
            CellInfo *ff2 = lts->cells[(half << 7) | (i << 4) | BEL_FF2];
            for (int j = 0; j < 2; j++) {
                CellInfo *ff = (j == 1) ? ff2 : ff1;
                if (ff == nullptr)
                    continue;
                std::string bname = get_bel_name(ff->bel);
                bool zrst = false, zinit = false;
                zinit = (int_or_default(ff->params, ctx->id("INIT"), 0) != 1);
                IdString srsig;
                if (ff->type == ctx->id("FDRE")) {
                    zrst = true;
                    srsig = ctx->id("R");
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, true);
                } else if (ff->type == ctx->id("FDSE")) {
                    zrst = false;
                    srsig = ctx->id("S");
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, true);
                } else if (ff->type == ctx->id("FDCE")) {
                    zrst = true;
                    srsig = ctx->id("CLR");
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, false);
                } else if (ff->type == ctx->id("FDPE")) {
                    zrst = false;
                    srsig = ctx->id("PRE");
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, false);
                } else {
                    NPNR_ASSERT_FALSE("unsupported FF type");
                }

                if (zinit)
                    out << tname << "." << hname << "." << bname << ".ZINIT" << std::endl;
                if (zrst)
                    out << tname << "." << hname << "." << bname << ".ZRST" << std::endl;

                SET_CHECK(is_clkinv, int_or_default(ff->params, ctx->id("IS_C_INVERTED")) == 1);
                SET_CHECK(is_srused, get_net_or_empty(ff, srsig) != nullptr);
                SET_CHECK(is_ceused, get_net_or_empty(ff, ctx->id("E")) != nullptr);

                // Input mux
                write_routing_bel(tname + "." + hname, ctx->getBelPinWire(ff->bel, ctx->id("D")));

                found_ff = true;
            }
        }
        if (is_latch)
            out << tname << "." << hname << ".LATCH";
        if (is_sync)
            out << tname << "." << hname << ".FFSYNC";
        if (is_clkinv)
            out << tname << "." << hname << ".CLKINV";
        if (is_srused)
            out << tname << "." << hname << ".SRUSED";
        if (is_ceused)
            out << tname << "." << hname << ".CEUSED";
    }
};
} // namespace

NEXTPNR_NAMESPACE_END