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
#include <boost/range/adaptor/reversed.hpp>
#include <fstream>
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
struct FasmBackend
{
    Context *ctx;
    std::ostream &out;
    std::vector<std::string> fasm_ctx;
    std::unordered_map<int, std::vector<PipId>> pips_by_tile;

    FasmBackend(Context *ctx, std::ostream &out) : ctx(ctx), out(out){};

    void push(const std::string &x) { fasm_ctx.push_back(x); }

    void pop() { fasm_ctx.pop_back(); }

    void pop(int N)
    {
        for (int i = 0; i < N; i++)
            fasm_ctx.pop_back();
    }
    bool last_was_blank = true;
    void blank()
    {
        if (!last_was_blank)
            out << std::endl;
        last_was_blank = true;
    }

    void write_prefix()
    {
        for (auto &x : fasm_ctx)
            out << x << ".";
        last_was_blank = false;
    }

    void write_bit(const std::string &name, bool value = true)
    {
        if (value) {
            write_prefix();
            out << name << std::endl;
        }
    }

    void write_vector(const std::string &name, const std::vector<bool> &value)
    {
        write_prefix();
        out << name << " = " << int(value.size()) << "'b";
        for (auto bit : boost::adaptors::reverse(value))
            out << (bit ? '1' : '0');
        out << std::endl;
    }

    struct PseudoPipKey
    {
        IdString tileType;
        IdString dest;
        IdString source;
        struct Hash
        {
            size_t operator()(const PseudoPipKey &key) const noexcept
            {
                std::size_t seed = 0;
                boost::hash_combine(seed, std::hash<IdString>()(key.tileType));
                boost::hash_combine(seed, std::hash<IdString>()(key.source));
                boost::hash_combine(seed, std::hash<IdString>()(key.dest));
                return seed;
            }
        };

        bool operator==(const PseudoPipKey &b) const
        {
            return std::tie(this->tileType, this->dest, this->source) == std::tie(b.tileType, b.dest, b.source);
        }
    };

    std::unordered_map<PseudoPipKey, std::vector<std::string>, PseudoPipKey::Hash> pp_config;
    void get_pseudo_pip_data()
    {
        /*
         * Create the mapping from pseudo pip tile type, source wire, and dest wire, to
         * the config bits set when that pseudo pip is used
         */
        for (std::string s : {"L", "R"})
            for (std::string s2 : {"", "_TBYTESRC", "_TBYTETERM"})
                for (std::string i : {"0", "1"}) {
                    pp_config[{ctx->id(s + "IOI3" + s2), ctx->id(s + "IOI_OLOGIC" + i + "_OQ"),
                               ctx->id("IOI_OLOGIC" + i + "_D1")}] = {
                            "OLOGIC_Y" + i + ".OMUX.D1", "OLOGIC_Y" + i + ".OQUSED", "OLOGIC_Y" + i + ".OQUSED",
                            "OLOGIC_Y" + i + ".OSERDESE.DATA_RATE_TQ.BUF"};
                    pp_config[{ctx->id(s + "IOI3" + s2), ctx->id("IOI_ILOGIC" + i + "_O"),
                               ctx->id(s + "IOI_ILOGIC" + i + "_D")}] = {"IDELAY_Y" + i + ".IDELAY_TYPE_FIXED",
                                                                         "ILOGIC_Y" + i + ".ZINV_D"};
                }

        for (std::string s1 : {"TOP", "BOT"}) {
            for (std::string s2 : {"L", "R"}) {
                for (int i = 0; i < 8; i++) {
                    std::string ii = std::to_string(i);
                    std::string hck = s2 + ii;
                    std::string buf = std::string((s2 == "R") ? "X1Y" : "X0Y") + ii;
                    pp_config[{ctx->id("CLK_HROW_" + s1 + "_R"), ctx->id("CLK_HROW_CK_HCLK_OUT_" + hck),
                               ctx->id("CLK_HROW_CK_MUX_OUT_" + hck)}] = {"BUFHCE.BUFHCE_" + buf + ".IN_USE",
                                                                          "BUFHCE.BUFHCE_" + buf + ".ZINV_CE"};
                }
            }

            for (int i = 0; i < 16; i++) {
                std::string ii = std::to_string(i);
                pp_config[{ctx->id("CLK_BUFG_" + s1 + "_R"), ctx->id("CLK_BUFG_BUFGCTRL" + ii + "_O"),
                           ctx->id("CLK_BUFG_BUFGCTRL" + ii + "_I0")}] = {
                        "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".IN_USE", "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".IS_IGNORE1_INVERTED",
                        "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".ZINV_CE0", "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".ZINV_S0"};
                pp_config[{ctx->id("CLK_BUFG_" + s1 + "_R"), ctx->id("CLK_BUFG_BUFGCTRL" + ii + "_O"),
                           ctx->id("CLK_BUFG_BUFGCTRL" + ii + "_I1")}] = {
                        "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".IN_USE", "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".IS_IGNORE0_INVERTED",
                        "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".ZINV_CE1", "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".ZINV_S1"};
            }
        }
    }

    void write_pip(PipId pip, NetInfo *net)
    {
        pips_by_tile[pip.tile].push_back(pip);

        auto src_intent = ctx->wireIntent(ctx->getPipSrcWire(pip));
        if (src_intent == ID_PSEUDO_GND || src_intent == ID_PSEUDO_VCC)
            return;

        auto &pd = ctx->locInfo(pip).pip_data[pip.index];
        if (pd.flags != PIP_TILE_ROUTING)
            return;

        IdString src = IdString(ctx->locInfo(pip).wire_data[pd.src_index].name);
        IdString dst = IdString(ctx->locInfo(pip).wire_data[pd.dst_index].name);

        PseudoPipKey ppk{IdString(ctx->locInfo(pip).type), dst, src};

        if (pp_config.count(ppk)) {
            auto &pp = pp_config.at(ppk);
            for (auto &c : pp)
                out << get_tile_name(pip.tile) << "." << c << std::endl;
            if (!pp.empty())
                last_was_blank = false;
        } else {
            out << get_tile_name(pip.tile) << ".";
            out << IdString(ctx->locInfo(pip).wire_data[pd.dst_index].name).str(ctx) << ".";
            out << IdString(ctx->locInfo(pip).wire_data[pd.src_index].name).str(ctx) << std::endl;
            last_was_blank = false;
        }
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

    void write_routing_bel(WireId dst_wire)
    {
        for (auto pip : ctx->getPipsUphill(dst_wire)) {
            if (ctx->getBoundPipNet(pip) != nullptr) {
                auto &pd = ctx->locInfo(pip).pip_data[pip.index];
                write_prefix();
                out << IdString(pd.bel).c_str(ctx) << "." << IdString(pd.extra_data).c_str(ctx);
                out << std::endl;
            }
        }
    }

    // Process flipflops in a half-tile
    void write_ffs_config(int tile, int half)
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
        push(tname);
        push(get_half_name(half, tname.find("CLBLM") != std::string::npos));

        auto lts = ctx->tileStatus[tile].lts;
        if (lts == nullptr)
            return;

        for (int i = 0; i < 4; i++) {
            CellInfo *ff1 = lts->cells[(half << 6) | (i << 4) | BEL_FF];
            CellInfo *ff2 = lts->cells[(half << 6) | (i << 4) | BEL_FF2];
            for (int j = 0; j < 2; j++) {
                CellInfo *ff = (j == 1) ? ff2 : ff1;
                if (ff == nullptr)
                    continue;
                push(get_bel_name(ff->bel));
                bool zrst = false, zinit = false;
                zinit = (int_or_default(ff->params, ctx->id("INIT"), 0) != 1);
                IdString srsig;
                std::string type = str_or_default(ff->attrs, ctx->id("X_ORIG_TYPE"), "");
                if (type == "FDRE") {
                    zrst = true;
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, true);
                } else if (type == "FDSE") {
                    zrst = false;
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, true);
                } else if (type == "FDCE") {
                    zrst = true;
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, false);
                } else if (type == "FDPE") {
                    zrst = false;
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, false);
                } else {
                    NPNR_ASSERT_FALSE("unsupported FF type");
                }

                write_bit("ZINI", zinit);
                write_bit("ZRST", zrst);

                pop();
                SET_CHECK(is_clkinv, int_or_default(ff->params, ctx->id("IS_C_INVERTED")) == 1);

                NetInfo *sr = get_net_or_empty(ff, ctx->id("SR")), *ce = get_net_or_empty(ff, ctx->id("CE"));

                SET_CHECK(is_srused, sr != nullptr && sr->name != ctx->id("$PACKER_GND_NET"));
                SET_CHECK(is_ceused, ce != nullptr && ce->name != ctx->id("$PACKER_VCC_NET"));

                // Input mux
                write_routing_bel(ctx->getBelPinWire(ff->bel, ctx->id("D")));

                found_ff = true;
            }
        }
        write_bit("LATCH", is_latch);
        write_bit("FFSYNC", is_sync);
        write_bit("CLKINV", is_clkinv);
        write_bit("SRUSEDMUX", is_srused);
        write_bit("CEUSEDMUX", is_ceused);
        pop(2);
    }

    // Get a named wire in the same site as a bel
    WireId get_site_wire(BelId site_bel, std::string name)
    {
        WireId ret;
        auto &l = ctx->locInfo(site_bel);
        auto &bd = l.bel_data[site_bel.index];
        IdString name_id = ctx->id(name);
        for (int i = 0; i < l.num_wires; i++) {
            auto &wd = l.wire_data[i];
            if (wd.site == bd.site && wd.name == name_id.index) {
                ret.tile = site_bel.tile;
                ret.index = i;
                break;
            }
        }
        return ret;
    }

    // Process LUTs and associated functionality in a half
    void write_luts_config(int tile, int half)
    {
        bool wa7_used = false, wa8_used = false;

        std::string tname = get_tile_name(tile);
        push(tname);
        push(get_half_name(half, tname.find("CLBLM") != std::string::npos));

        auto lts = ctx->tileStatus[tile].lts;
        if (lts == nullptr)
            return;

        BelId bel_in_half =
                ctx->getBelByLocation(Loc(tile % ctx->chip_info->width, tile / ctx->chip_info->width, half << 6));

        for (int i = 0; i < 4; i++) {
            CellInfo *lut6 = lts->cells[(half << 6) | (i << 4) | BEL_6LUT];
            CellInfo *lut5 = lts->cells[(half << 6) | (i << 4) | BEL_5LUT];
            if (lut6 == nullptr && lut5 == nullptr)
                continue;
            std::string lutname = std::string("") + ("ABCD"[i]) + std::string("LUT");
            push(lutname);
            // Write LUT initialisation
            write_vector("INIT[63:0]", get_lut_init(lut6, lut5));

            // Write LUT mode config
            bool is_small = false, is_ram = false, is_srl = false;
            for (int j = 0; j < 2; j++) {
                CellInfo *lut = (j == 1) ? lut5 : lut6;
                if (lut == nullptr)
                    continue;
                if (lut->type == ctx->id("RAMD64E") || lut->type == ctx->id("RAMS64E")) {
                    is_ram = true;
                } else if (lut->type == ctx->id("RAMD32E") || lut->type == ctx->id("RAMS32E")) {
                    is_ram = true;
                    is_small = true;
                } else if (lut->type == ctx->id("SRL16E")) {
                    is_srl = true;
                    is_small = true;
                } else if (lut->type == ctx->id("SRLC32E")) {
                    is_srl = true;
                }
                wa7_used |= (get_net_or_empty(lut, ctx->id("WA7")) != nullptr);
                wa8_used |= (get_net_or_empty(lut, ctx->id("WA8")) != nullptr);
            }
            write_bit("SMALL", is_small);
            write_bit("RAM", is_ram);
            write_bit("SRL", is_srl);
            pop();
            write_routing_bel(get_site_wire(bel_in_half, std::string("") + ("ABCD"[i]) + std::string("MUX")));
        }
        write_bit("WA7USED", wa7_used);
        write_bit("WA8USED", wa8_used);
        pop(2);
    }

    void write_logic()
    {
        std::set<int> used_logic_tiles;
        for (auto &cell : ctx->cells) {
            if (ctx->isLogicTile(cell.second->bel))
                used_logic_tiles.insert(cell.second->bel.tile);
        }
        for (int tile : used_logic_tiles) {
            write_luts_config(tile, 0);
            write_luts_config(tile, 1);
            write_ffs_config(tile, 0);
            write_ffs_config(tile, 1);
            blank();
        }
    }

    void write_routing()
    {
        get_pseudo_pip_data();
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            for (auto &w : ni->wires) {
                if (w.second.pip != PipId())
                    write_pip(w.second.pip, ni);
            }
            blank();
        }
    }

    void write_io_config(CellInfo *pad)
    {
        NetInfo *pad_net = get_net_or_empty(pad, ctx->id("PAD"));
        NPNR_ASSERT(pad_net != nullptr);
        std::string iostandard = str_or_default(pad->attrs, ctx->id("IOSTANDARD"), "LVCMOS33");
        std::string pulltype = str_or_default(pad->attrs, ctx->id("PULLTYPE"), "NONE");
        std::string slew = str_or_default(pad->attrs, ctx->id("SLEW"), "SLOW");

        Loc ioLoc = ctx->getSiteLocInTile(pad->bel);
        bool is_output = false, is_input = false;
        if (pad_net->driver.cell != nullptr)
            is_output = true;
        for (auto &usr : pad_net->users)
            if (usr.cell->type.str(ctx).find("INBUF") != std::string::npos)
                is_input = true;
        push(get_tile_name(pad->bel.tile));
        push("IOB_Y" + std::to_string(1 - ioLoc.y));
        if (is_output) {
            if (iostandard == "LVCMOS33" || iostandard == "LVTTL")
                write_bit("LVCMOS33_LVTTL.DRIVE.I12_I16");

            if (slew == "SLOW")
                write_bit("LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL_SSTL135.SLEW.SLOW");
            else if (iostandard == "SSTL135")
                write_bit("SSTL135.SLEW.FAST");
            else
                write_bit("LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL.SLEW.FAST");
        }
        if (is_input) {
            if (iostandard == "LVCMOS33" || iostandard == "LVTTL" || iostandard == "LVCMOS25")
                write_bit("LVCMOS25_LVCMOS33_LVTTL.IN");
            if (!is_output)
                write_bit("LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL_SSTL135.IN_ONLY");
        }
        write_bit("PULLTYPE." + pulltype);
        pop(2);
    }

    void write_io()
    {
        for (auto cell : sorted(ctx->cells)) {
            CellInfo *ci = cell.second;
            if (ci->type == ctx->id("PAD")) {
                write_io_config(ci);
                blank();
            }
        }
    }

    std::vector<std::string> used_wires_starting_with(int tile, const std::string &prefix, bool is_source)
    {
        std::vector<std::string> wires;
        if (!pips_by_tile.count(tile))
            return wires;
        for (auto pip : pips_by_tile[tile]) {
            auto &pd = ctx->locInfo(pip).pip_data[pip.index];
            int wire_index = is_source ? pd.src_index : pd.dst_index;
            std::string wire = IdString(ctx->locInfo(pip).wire_data[wire_index].name).str(ctx);
            if (boost::starts_with(wire, prefix))
                wires.push_back(wire);
        }
        return wires;
    }

    void write_clocking()
    {
        auto tt = ctx->getTilesAndTypes();
        std::string name, type;

        std::set<std::string> all_gclk;

        for (int tile = 0; tile < int(tt.size()); tile++) {
            std::tie(name, type) = tt.at(tile);
            push(name);
            if (type == "HCLK_L" || type == "HCLK_R") {
                auto used_sources = used_wires_starting_with(tile, "HCLK_CK_", true);
                push("ENABLE_BUFFER");
                for (auto s : used_sources)
                    write_bit(s);
                pop();
            } else if (boost::starts_with(type, "CLK_HROW")) {
                auto used_gclk = used_wires_starting_with(tile, "CLK_HROW_R_CK_GCLK", true);
                auto used_ck_in = used_wires_starting_with(tile, "CLK_HROW_CK_IN", true);
                for (auto s : used_gclk) {
                    write_bit(s + "_ACTIVE");
                    all_gclk.insert(s.substr(s.find("GCLK")));
                }
                for (auto s : used_ck_in)
                    write_bit(s + "_ACTIVE");
            } else if (boost::starts_with(type, "HCLK_CMT")) {
                auto used_ccio = used_wires_starting_with(tile, "HCLK_CMT_CCIO", true);
                for (auto s : used_ccio) {
                    write_bit(s + "_ACTIVE");
                    write_bit(s + "_USED");
                }
            }
            pop();
            blank();
        }
    }

    void write_fasm()
    {
        write_logic();
        write_io();
        write_routing();
        write_clocking();
    }
};

} // namespace

void Arch::writeFasm(const std::string &filename)
{
    std::ofstream out(filename);
    if (!out)
        log_error("failed to open file %s for writing (%s)\n", filename.c_str(), strerror(errno));

    FasmBackend be(getCtx(), out);
    be.write_fasm();
}

NEXTPNR_NAMESPACE_END