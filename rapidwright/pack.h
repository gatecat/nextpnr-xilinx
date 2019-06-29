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
#include "pins.h"

NEXTPNR_NAMESPACE_BEGIN

struct USPacker
{
    Context *ctx;

    // Generic cell transformation
    // Given cell name map and port map
    // If port name is not found in port map; it will be copied as-is but stripping []
    struct XFormRule
    {
        IdString new_type;
        std::unordered_map<IdString, IdString> port_xform;
        std::unordered_map<IdString, std::vector<IdString>> port_multixform;
        std::unordered_map<IdString, IdString> param_xform;
        std::vector<std::pair<IdString, std::string>> set_attrs;
        std::vector<std::pair<IdString, Property>> set_params;
    };

    // Distributed RAM control set
    struct DRAMControlSet
    {
        std::vector<NetInfo *> wa;
        NetInfo *wclk, *we;
        bool wclk_inv;

        bool operator==(const DRAMControlSet &other) const
        {
            return wa == other.wa && wclk == other.wclk && we == other.we && wclk_inv == other.wclk_inv;
        }
        bool operator!=(const DRAMControlSet &other) const
        {
            return wa != other.wa && wclk != other.wclk && we != other.we && wclk_inv != other.wclk_inv;
        }
    };

    struct DRAMControlSetHash
    {
        size_t operator()(const DRAMControlSet &dcs) const
        {
            std::size_t seed = 0;
            boost::hash_combine(seed, std::hash<size_t>()(dcs.wa.size()));
            for (auto abit : dcs.wa)
                boost::hash_combine(seed, std::hash<IdString>()(abit == nullptr ? IdString() : abit->name));
            boost::hash_combine(seed, std::hash<IdString>()(dcs.wclk == nullptr ? IdString() : dcs.wclk->name));
            boost::hash_combine(seed, std::hash<IdString>()(dcs.we == nullptr ? IdString() : dcs.we->name));
            boost::hash_combine(seed, std::hash<bool>()(dcs.wclk_inv));
            return seed;
        }
    };

    struct DRAMType
    {
        int abits;
        int dbits;
        int rports;
    };

    struct CarryGroup
    {
        std::vector<CellInfo *> muxcys;
        std::vector<CellInfo *> xorcys;
    };

    std::unordered_set<IdString> packed_cells;
    std::vector<std::unique_ptr<CellInfo>> new_cells;

    // General helper functions
    void flush_cells();

    void xform_cell(const std::unordered_map<IdString, XFormRule> &rules, CellInfo *ci);
    void generic_xform(const std::unordered_map<IdString, XFormRule> &rules, bool print_summary = false);

    std::unique_ptr<CellInfo> feed_through_lut(NetInfo *net, const std::vector<PortRef> &feed_users);
    IdString int_name(IdString base, const std::string &postfix);
    NetInfo *create_internal_net(IdString base, const std::string &postfix);
    void rename_net(IdString old, IdString newname);

    // LUTs & FFs
    void pack_luts();
    void pack_ffs();
    void pack_lutffs();

    // DistRAM
    std::unordered_map<IdString, XFormRule> dram_rules;
    CellInfo *create_dram_lut(const std::string &name, CellInfo *base, const DRAMControlSet &ctrlset,
                              std::vector<NetInfo *> address, NetInfo *di, NetInfo *dout, int z);
    void pack_dram();

    // Constant pins
    std::unordered_map<IdString, std::unordered_map<IdString, bool>> tied_pins;
    std::unordered_map<IdString, std::unordered_set<IdString>> invertible_pins;
    void pack_constants();

    // Carries
    bool has_illegal_fanout(NetInfo *carry);
    void pack_carries();

    // IO
    NetInfo *invert_net(NetInfo *toinv);
    CellInfo *insert_ibufctrl(IdString name, NetInfo *i, NetInfo *o);
    CellInfo *insert_inbuf(IdString name, NetInfo *pad, NetInfo *o);
    CellInfo *insert_obuf(IdString name, IdString type, NetInfo *i, NetInfo *o, NetInfo *tri = nullptr);
    CellInfo *insert_diffinbuf(IdString name, const std::array<NetInfo *, 2> &i, NetInfo *o);

    std::unordered_map<IdString, std::unordered_set<IdString>> toplevel_ports;
    std::pair<CellInfo *, PortRef> insert_pad_and_buf(CellInfo *npnr_io);
    CellInfo *create_iobuf(CellInfo *npnr_io, IdString &top_port);
    void decompose_iob(CellInfo *xil_iob);
    void pack_io();

    // BRAM
    void pack_bram();

    int autoidx = 0;
};

NEXTPNR_NAMESPACE_END