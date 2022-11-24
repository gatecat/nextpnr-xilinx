/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@symbioticeda.com>
 *  Copyright (C) 2018-19  David Shah <david@symbioticeda.com>
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
#include <boost/range/adaptor/reversed.hpp>
#include <cmath>
#include <cstring>
#include <queue>
#include "log.h"
#include "nextpnr.h"
#include "placer1.h"
#include "placer_heap.h"
#include "router1.h"
#include "router2.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

static std::pair<std::string, std::string> split_identifier_name(const std::string &name)
{
    size_t first_slash = name.find('/');
    NPNR_ASSERT(first_slash != std::string::npos);
    return std::make_pair(name.substr(0, first_slash), name.substr(first_slash + 1));
};

static std::pair<std::string, std::string> split_identifier_name_dot(const std::string &name)
{
    size_t first_dot = name.find('.');
    NPNR_ASSERT(first_dot != std::string::npos);
    return std::make_pair(name.substr(0, first_dot), name.substr(first_dot + 1));
};

// -----------------------------------------------------------------------

void IdString::initialize_arch(const BaseCtx *ctx)
{
#define X(t) initialize_add(ctx, #t, ID_##t);

#include "constids.inc"

#undef X
}

// -----------------------------------------------------------------------

static const ChipInfoPOD *get_chip_info(const RelPtr<ChipInfoPOD> *ptr) { return ptr->get(); }

Arch::Arch(ArchArgs args) : args(args)
{
    try {
        blob_file.open(args.chipdb);
        if (args.chipdb.empty() || !blob_file.is_open())
            log_error("Unable to read chipdb %s\n", args.chipdb.c_str());
        const char *blob = reinterpret_cast<const char *>(blob_file.data());
        chip_info = get_chip_info(reinterpret_cast<const RelPtr<ChipInfoPOD> *>(blob));
    } catch (...) {
        log_error("Unable to read chipdb %s\n", args.chipdb.c_str());
    }

    for (int i = 0; i < chip_info->extra_constids->bba_id_count; i++) {
        // log_info("%s %d\n", chip_info->extra_constids->bba_ids[i].get(), int(idstring_idx_to_str->size()));
        IdString::initialize_add(this, chip_info->extra_constids->bba_ids[i].get(),
                                 i + chip_info->extra_constids->known_id_count);
    }

    if (std::string(chip_info->name.get()).find("xc7") == 0)
        xc7 = true;
    else
        xc7 = false;

    tileStatus.resize(chip_info->num_tiles);
    for (int i = 0; i < chip_info->num_tiles; i++) {
        tileStatus[i].boundcells.resize(chip_info->tile_types[chip_info->tile_insts[i].type].num_bels);
        tileStatus[i].sitevariant.resize(chip_info->tile_insts[i].num_sites);
    }

    if (xc7)
        setup_pip_blacklist();
}

// -----------------------------------------------------------------------

std::string Arch::getChipName() const { return chip_info->name.get(); }

// -----------------------------------------------------------------------

IdString Arch::archArgsToId(ArchArgs args) const { return IdString(); }

// -----------------------------------------------------------------------

void Arch::setup_byname() const
{
    if (tile_by_name.empty()) {
        for (int i = 0; i < chip_info->num_tiles; i++) {
            tile_by_name[chip_info->tile_insts[i].name.get()] = i;
        }
    }

    if (site_by_name.empty()) {
        for (int i = 0; i < chip_info->num_tiles; i++) {
            auto &tile = chip_info->tile_insts[i];
            for (int j = 0; j < tile.num_sites; j++) {
                auto name = tile.site_insts[j].name.get();
                site_by_name[name] = std::make_pair(i, j);
            }
        }
    }
}

BelId Arch::getBelByName(IdString name) const
{
    BelId ret;

    setup_byname();

    auto split = split_identifier_name(name.str(this));
    if (site_by_name.count(split.first)) {
        int tile, site;
        std::tie(tile, site) = site_by_name.at(split.first);
        auto &tile_info = chip_info->tile_types[chip_info->tile_insts[tile].type];
        IdString belname = id(split.second);
        for (int i = 0; i < tile_info.num_bels; i++) {
            if (tile_info.bel_data[i].site == site && tile_info.bel_data[i].name == belname.index) {
                ret.tile = tile;
                ret.index = i;
                break;
            }
        }
    } else {
        int tile = tile_by_name.at(split.first);
        auto &tile_info = chip_info->tile_types[chip_info->tile_insts[tile].type];
        IdString belname = id(split.second);
        for (int i = 0; i < tile_info.num_bels; i++) {
            if (tile_info.bel_data[i].name == belname.index) {
                ret.tile = tile;
                ret.index = i;
                break;
            }
        }
    }

    return ret;
}

BelRange Arch::getBelsByTile(int x, int y) const
{
    BelRange br;

    br.b.cursor_tile = y * chip_info->width + x;
    br.e.cursor_tile = y * chip_info->width + x;
    br.b.cursor_index = 0;
    br.e.cursor_index = chip_info->tile_types[chip_info->tile_insts[br.b.cursor_tile].type].num_bels;
    br.b.chip = chip_info;
    br.e.chip = chip_info;
    if (br.e.cursor_index == -1)
        ++br.e.cursor_index;
    else
        ++br.e;
    return br;
}

WireId Arch::getBelPinWire(BelId bel, IdString pin) const
{
    WireId ret;

    NPNR_ASSERT(bel != BelId());

    int num_bel_wires = locInfo(bel).bel_data[bel.index].num_bel_wires;
    const BelWirePOD *bel_wires = locInfo(bel).bel_data[bel.index].bel_wires.get();
    for (int i = 0; i < num_bel_wires; i++)
        if (bel_wires[i].port == pin.index) {
#if 0
            WireId tmp;
            tmp.tile = bel.tile;
            tmp.index = bel_wires[i].wire_index;
            log_info("%s\n", getWireName(tmp).c_str(this));
#endif
            return canonicalWireId(chip_info, bel.tile, bel_wires[i].wire_index);
        }

    return ret;
}

PortType Arch::getBelPinType(BelId bel, IdString pin) const
{
    NPNR_ASSERT(bel != BelId());

    int num_bel_wires = locInfo(bel).bel_data[bel.index].num_bel_wires;
    const BelWirePOD *bel_wires = locInfo(bel).bel_data[bel.index].bel_wires.get();

    for (int i = 0; i < num_bel_wires; i++)
        if (bel_wires[i].port == pin.index)
            return PortType(bel_wires[i].type);

    return PORT_INOUT;
}

// -----------------------------------------------------------------------

WireId Arch::getWireByName(IdString name) const
{
    if (wire_by_name_cache.count(name))
        return wire_by_name_cache.at(name);
    WireId ret;
    setup_byname();

    const std::string &s = name.str(this);
    if (s.substr(0, 9) == "SITEWIRE/") {
        auto sp2 = split_identifier_name(s.substr(9));
        int tile, site;
        std::tie(tile, site) = site_by_name.at(sp2.first);
        auto &tile_info = chip_info->tile_types[chip_info->tile_insts[tile].type];
        IdString wirename = id(sp2.second);
        for (int i = 0; i < tile_info.num_wires; i++) {
            if (tile_info.wire_data[i].site == site && tile_info.wire_data[i].name == wirename.index) {
                ret.tile = tile;
                ret.index = i;
                break;
            }
        }
    } else {
        auto sp = split_identifier_name(s);
        int tile = tile_by_name.at(sp.first);
        auto &tile_info = chip_info->tile_types[chip_info->tile_insts[tile].type];
        IdString wirename = id(sp.second);
        for (int i = 0; i < tile_info.num_wires; i++) {
            if (tile_info.wire_data[i].site == -1 && tile_info.wire_data[i].name == wirename.index) {
                ret.tile = tile;
                ret.index = i;
                break;
            }
        }
    }

    wire_by_name_cache[name] = ret;

    return ret;
}

IdString Arch::getWireType(WireId wire) const { return IdString(wireIntent(wire)); }
std::vector<std::pair<IdString, std::string>> Arch::getWireAttrs(WireId wire) const
{
    return {{id("INTENT"), IdString(wireIntent(wire)).str(this)}};
}

// -----------------------------------------------------------------------

PipId Arch::getPipByName(IdString name) const
{
    if (pip_by_name_cache.count(name))
        return pip_by_name_cache.at(name);
    PipId ret;
    setup_byname();

    const std::string &s = name.str(this);
    if (s.substr(0, 8) == "SITEPIP/") {
        auto sp2 = split_identifier_name(s.substr(8));
        int tile, site;
        std::tie(tile, site) = site_by_name.at(sp2.first);
        auto &tile_info = chip_info->tile_types[chip_info->tile_insts[tile].type];
        auto sp3 = split_identifier_name(sp2.second);
        IdString belname = id(sp3.first), pinname = id(sp3.second);
        for (int i = 0; i < tile_info.num_pips; i++) {
            if (tile_info.pip_data[i].site == site && tile_info.pip_data[i].bel == belname.index &&
                tile_info.pip_data[i].extra_data == pinname.index) {
                ret.tile = tile;
                ret.index = i;
                break;
            }
        }
    } else {
        auto sp = split_identifier_name(s);
        int tile = tile_by_name.at(sp.first);
        auto &tile_info = chip_info->tile_types[chip_info->tile_insts[tile].type];

        auto spn = split_identifier_name_dot(sp.second);
        int fromwire = std::stoi(spn.first), towire = std::stoi(spn.second);

        for (int i = 0; i < tile_info.num_pips; i++) {
            if (tile_info.pip_data[i].site == -1 && tile_info.pip_data[i].src_index == fromwire &&
                tile_info.pip_data[i].dst_index == towire) {
                ret.tile = tile;
                ret.index = i;
                break;
            }
        }
    }

    pip_by_name_cache[name] = ret;

    return ret;
}

IdString Arch::getPipName(PipId pip) const
{
    NPNR_ASSERT(pip != PipId());
    if (locInfo(pip).pip_data[pip.index].site != -1 && locInfo(pip).pip_data[pip.index].flags == PIP_SITE_INTERNAL &&
        locInfo(pip).pip_data[pip.index].bel != -1) {
        return id(std::string("SITEPIP/") +
                  chip_info->tile_insts[pip.tile].site_insts[locInfo(pip).pip_data[pip.index].site].name.get() +
                  std::string("/") + IdString(locInfo(pip).pip_data[pip.index].bel).str(this) + "/" +
                  IdString(locInfo(pip).wire_data[locInfo(pip).pip_data[pip.index].src_index].name).str(this));
    } else {
        return id(std::string(chip_info->tile_insts[pip.tile].name.get()) + "/" +
                  std::to_string(locInfo(pip).pip_data[pip.index].src_index) + "." +
                  std::to_string(locInfo(pip).pip_data[pip.index].dst_index));
    }
}

void Arch::setup_pip_blacklist()
{
    for (int i = 0; i < chip_info->num_tiletypes; i++) {
        auto &td = chip_info->tile_types[i];
        std::string type = IdString(td.type).str(this);
        if (boost::starts_with(type, "HCLK_CMT")) {
            for (int j = 0; j < td.num_pips; j++) {
                auto &pd = td.pip_data[j];
                std::string dest_name = IdString(td.wire_data[pd.dst_index].name).str(this);
                if (dest_name.find("FREQ_REF") != std::string::npos)
                    blacklist_pips[td.type].insert(j);
            }
        } else if (boost::starts_with(type, "CMT_TOP_L_LOWER")) {
            for (int j = 0; j < td.num_pips; j++) {
                blacklist_pips[td.type].insert(j);
            }
        } else if (boost::starts_with(type, "CLK_HROW_TOP")) {
            for (int j = 0; j < td.num_pips; j++) {
                auto &pd = td.pip_data[j];
                std::string dest_name = IdString(td.wire_data[pd.dst_index].name).str(this);
                std::string src_name = IdString(td.wire_data[pd.src_index].name).str(this);

                if (dest_name.find("CK_BUFG_CASCO") != std::string::npos &&
                    src_name.find("CK_BUFG_CASCIN") != std::string::npos)
                    blacklist_pips[td.type].insert(j);
            }
        } else if (boost::starts_with(type, "HCLK_IOI")) {
            for (int j = 0; j < td.num_pips; j++) {
                auto &pd = td.pip_data[j];
                std::string dest_name = IdString(td.wire_data[pd.dst_index].name).str(this);
                std::string src_name = IdString(td.wire_data[pd.src_index].name).str(this);

                if (dest_name.find("RCLK_BEFORE_DIV") != std::string::npos &&
                    src_name.find("IMUX") != std::string::npos)
                    blacklist_pips[td.type].insert(j);
            }
        } else if (type.find("IOI") != std::string::npos) {
            for (int j = 0; j < td.num_pips; j++) {
                auto &pd = td.pip_data[j];
                std::string dest_name = IdString(td.wire_data[pd.dst_index].name).str(this);
                std::string src_name = IdString(td.wire_data[pd.src_index].name).str(this);

                if (dest_name.find("CLKB") != std::string::npos && src_name.find("IMUX22") != std::string::npos)
                    blacklist_pips[td.type].insert(j);
                if (dest_name.find("OCLKB") != std::string::npos && src_name.find("IOI_OCLK_") != std::string::npos)
                    blacklist_pips[td.type].insert(j);
                if (dest_name.find("OCLKM") != std::string::npos && src_name.find("IMUX31") != std::string::npos)
                    blacklist_pips[td.type].insert(j);
            }
        } else if (boost::starts_with(type, "CMT_TOP_R")) {
            for (int j = 0; j < td.num_pips; j++) {
                auto &pd = td.pip_data[j];
                std::string dest_name = IdString(td.wire_data[pd.dst_index].name).str(this);
                std::string src_name = IdString(td.wire_data[pd.src_index].name).str(this);

                if (dest_name.find("PLLOUT_CLK_FREQ_BB_REBUFOUT") != std::string::npos)
                    blacklist_pips[td.type].insert(j);
                if (dest_name.find("MMCM_CLK_FREQ_BB") != std::string::npos)
                    blacklist_pips[td.type].insert(j);
            }
        }
    }
}

IdString Arch::getPipType(PipId pip) const { return id("PIP"); }

std::vector<std::pair<IdString, std::string>> Arch::getPipAttrs(PipId pip) const { return {}; }

// -----------------------------------------------------------------------

std::vector<IdString> Arch::getBelPins(BelId bel) const
{
    std::vector<IdString> ret;
    NPNR_ASSERT(bel != BelId());

    int num_bel_wires = locInfo(bel).bel_data[bel.index].num_bel_wires;
    const BelWirePOD *bel_wires = locInfo(bel).bel_data[bel.index].bel_wires.get();

    for (int i = 0; i < num_bel_wires; i++) {
        IdString id;
        id.index = bel_wires[i].port;
        ret.push_back(id);
    }

    return ret;
}

BelId Arch::getBelByLocation(Loc loc) const
{
    BelId bi;
    if (loc.x >= chip_info->width || loc.y >= chip_info->height)
        return BelId();
    bi.tile = (loc.y * chip_info->width + loc.x);
    auto &li = locInfo(bi);
    for (int i = 0; i < li.num_bels; i++) {
        if (li.bel_data[i].z == loc.z) {
            bi.index = i;
            return bi;
        }
    }
    return BelId();
}

std::vector<std::pair<IdString, std::string>> Arch::getBelAttrs(BelId bel) const { return {}; }

// -----------------------------------------------------------------------

delay_t Arch::estimateDelay(WireId src, WireId dst, bool debug) const
{
    if (src == dst)
        return 0;
    int src_x, src_y, dst_x, dst_y;
    int src_intent = wireIntent(src), dst_intent = wireIntent(dst);
    // if (src_intent == ID_PSEUDO_GND || dst_intent == ID_PSEUDO_VCC)
    //    return 500;
    int dst_tile = dst.tile == -1 ? chip_info->nodes[dst.index].tile_wires[0].tile : dst.tile;
    int src_tile = src.tile == -1 ? chip_info->nodes[src.index].tile_wires[0].tile : src.tile;

    if (sink_locs.count(dst)) {
        dst_x = sink_locs.at(dst).x;
        dst_y = sink_locs.at(dst).y;
        if (src_tile == dst_tile || (sink_locs.count(src) && (sink_locs.at(dst) == sink_locs.at(src)))) {
            return 1000;
        }
    } else if (dst.tile != -1 && chip_info->tile_insts[dst.tile].num_sites > 0) {
        auto &site = chip_info->tile_insts[dst.tile].site_insts[wireInfo(dst).site != -1 ? wireInfo(dst).site : 0];
        if (site.inter_x != -1) {
            dst_x = site.inter_x;
            dst_y = site.inter_y;
        } else {
            dst_x = dst.tile % chip_info->width;
            dst_y = dst.tile / chip_info->width;
        }
    } else {
        dst_x = dst_tile % chip_info->width;
        dst_y = dst_tile / chip_info->width;
    }

    if (src.tile == -1) {
        if (src_intent == ID_PSEUDO_GND || src_intent == ID_PSEUDO_VCC) {
            if (gnd_glbl == IdString()) {
                gnd_glbl = id("PSEUDO_GND_WIRE_GLBL");
                gnd_row = id("PSEUDO_GND_WIRE_ROW");
                vcc_glbl = id("PSEUDO_VCC_WIRE_GLBL");
                vcc_row = id("PSEUDO_VCC_WIRE_ROW");
            }
            if (debug)
                log_info("%s %d %d\n", IdString(wireInfo(src).name).c_str(this), wireInfo(src).name, gnd_glbl.index);
            if (wireInfo(src).name == gnd_glbl.index || wireInfo(src).name == vcc_glbl.index)
                return 15000;

            src_x = src_tile % chip_info->width;
            src_y = src_tile / chip_info->width;
            if (wireInfo(src).name == gnd_row.index || wireInfo(src).name == vcc_row.index)
                src_x = chip_info->width / 2;
        } else {
            auto &src_n = chip_info->nodes[src.index];
            src_x = -1;
            src_y = -1;
            for (int i = 0; i < std::min(200, src_n.num_tile_wires); i++) {
                // Approximate the nearest location to dest
                int ti = src_n.tile_wires[i].tile;
                auto &tw = chip_info->tile_types[chip_info->tile_insts[ti].type].wire_data[src_n.tile_wires[i].index];
                if (tw.num_downhill == 0 && src_intent != ID_NODE_PINFEED)
                    continue;
                int tix = ti % chip_info->width, tiy = ti / chip_info->width;
                if (src_x == -1 || std::abs(tix - dst_x) < std::abs(src_x - dst_x))
                    src_x = tix;
                if (src_y == -1 || std::abs(tiy - dst_y) < std::abs(src_y - dst_y))
                    src_y = tiy;
            }
            if (src_x == -1) {
                src_x = chip_info->nodes[src.index].tile_wires[0].tile % chip_info->width;
                src_y = chip_info->nodes[src.index].tile_wires[0].tile / chip_info->width;
            }
        }

    } else if (src.tile != -1 && chip_info->tile_insts[src.tile].num_sites > 0) {
        auto &site = chip_info->tile_insts[src.tile].site_insts[wireInfo(src).site != -1 ? wireInfo(src).site : 0];
        if (site.inter_x != -1) {
            src_x = site.inter_x;
            src_y = site.inter_y;
        } else {
            src_x = src.tile % chip_info->width;
            src_y = src.tile / chip_info->width;
        }
    } else {
        src_x = src_tile % chip_info->width;
        src_y = src_tile / chip_info->width;
    }
    if (debug)
        log_info("    src (%d, %d) dst (%d, %d)\n", src_x, src_y, dst_x, dst_y);
    /*
        delay_t base = 150 * std::min(std::abs(dst_x - src_x), 30) + 40 * std::max(std::abs(dst_x - src_x) - 30, 0)
                +  150 * std::min(std::abs(dst_y - src_y), 10) + 60 * std::max(std::abs(dst_y - src_y)  - 10, 0)
                + 500;
        auto &srci = wireInfo(src);*/
    /*
    if (srci.intent == ID_NODE_HLONG || srci.intent == ID_NODE_VLONG)
        base -= 180;
    if (srci.intent == ID_NODE_HQUAD || srci.intent == ID_NODE_VQUAD || srci.intent == ID_NODE_DOUBLE)
        base -= 120;
    */
    delay_t base = 30 * std::min(std::abs(dst_x - src_x), 18) + 10 * std::max(std::abs(dst_x - src_x) - 18, 0) +
                   60 * std::min(std::abs(dst_y - src_y), 6) + 20 * std::max(std::abs(dst_y - src_y) - 6, 0) + 300;

    if (xc7)
        base = (base * 3) / 2;

    if (sink_locs.count(dst))
        base += 1000;
    if (src_intent == ID_NODE_PINFEED && dst_x == src_x && dst_y == src_y)
        base -= 200;
    else if ((src_intent == ID_NODE_LOCAL || src_intent == ID_NODE_PINBOUNCE) && dst_x == src_x && dst_y == src_y)
        base -= 100;
    if (src_intent == ID_NODE_CLE_OUTPUT)
        base -= 80;

    return base;
}

ArcBounds Arch::getRouteBoundingBox(WireId src, WireId dst) const
{
    int dst_tile = dst.tile == -1 ? chip_info->nodes[dst.index].tile_wires[0].tile : dst.tile;
    int src_tile = src.tile == -1 ? chip_info->nodes[src.index].tile_wires[0].tile : src.tile;

    int x0, x1, y0, y1;
    x0 = src_tile % chip_info->width;
    x1 = x0;
    y0 = src_tile / chip_info->width;
    y1 = y0;
    auto expand = [&](int x, int y) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    };

    expand(dst_tile % chip_info->width, dst_tile / chip_info->width);

    if (source_locs.count(src))
        expand(source_locs.at(src).x, source_locs.at(src).y);

    if (sink_locs.count(dst)) {
        expand(sink_locs.at(dst).x, sink_locs.at(dst).y);
    } else if (dst.tile != -1 && chip_info->tile_insts[dst.tile].num_sites > 0) {
        auto &site = chip_info->tile_insts[dst.tile].site_insts[wireInfo(dst).site != -1 ? wireInfo(dst).site : 0];
        if (site.inter_x != -1) {
            expand(site.inter_x, site.inter_y);
        }
    }

    if (src.tile != -1 && chip_info->tile_insts[src.tile].num_sites > 0) {
        auto &site = chip_info->tile_insts[dst.tile].site_insts[wireInfo(dst).site != -1 ? wireInfo(dst).site : 0];
        if (site.inter_x != -1) {
            expand(site.inter_x, site.inter_y);
        }
    }
    return {x0, y0, x1, y1};
}

delay_t Arch::getBoundingBoxCost(WireId src, WireId dst, int distance) const
{
    int src_intent = wireIntent(src);
    if (src.tile == -1 && (src_intent == ID_PSEUDO_GND || src_intent == ID_PSEUDO_VCC))
        return 0;
    if (distance < 5)
        return 0;
    return (distance - 5) * 0;
}

delay_t Arch::getWireRipupDelayPenalty(WireId wire) const
{
    if (wireIntent(wire) == ID_NODE_PINFEED)
        return (3 * getRipupDelayPenalty()) / 2;
    else
        return getRipupDelayPenalty();
}

delay_t Arch::predictDelay(const NetInfo *net_info, const PortRef &sink) const
{
    if (net_info->driver.cell == nullptr || net_info->driver.cell->bel == BelId() || sink.cell->bel == BelId())
        return 0;
    int src_x = net_info->driver.cell->bel.tile % chip_info->width,
        src_y = net_info->driver.cell->bel.tile / chip_info->width;

    int dst_x = sink.cell->bel.tile % chip_info->width, dst_y = sink.cell->bel.tile / chip_info->width;

    if (net_info->driver.cell->bel.tile == sink.cell->bel.tile) {
        Loc dl = getBelLocation(net_info->driver.cell->bel), sl = getBelLocation(sink.cell->bel);
        if ((dl.z >> 4) == (sl.z >> 4))
            return 0;
        else if ((dl.z & 0xF) == BEL_FF2)
            return 700; // penalize FF2 as it makes routing harder
        else
            return 150;
    } else {
        delay_t base = 30 * std::min(std::abs(dst_x - src_x), 18) + 10 * std::max(std::abs(dst_x - src_x) - 18, 0) +
                       60 * std::min(std::abs(dst_y - src_y), 6) + 20 * std::max(std::abs(dst_y - src_y) - 6, 0) + 300;

        if (xc7)
            base = (base * 3) / 2;
        return base;
    }
}

bool Arch::getBudgetOverride(const NetInfo *net_info, const PortRef &sink, delay_t &budget) const { return false; }

// -----------------------------------------------------------------------

bool Arch::place()
{
    std::string placer = str_or_default(settings, id("placer"), defaultPlacer);

    if (placer == "heap") {
        PlacerHeapCfg cfg(getCtx());
        cfg.criticalityExponent = 7;
        cfg.ioBufTypes.insert(id("IOB_IBUFCTRL"));
        cfg.ioBufTypes.insert(id("IOB_OUTBUF"));
        cfg.ioBufTypes.insert(id_PSEUDO_GND);
        cfg.ioBufTypes.insert(id_PSEUDO_VCC);
        cfg.alpha = 0.08;
        cfg.beta = 0.4;
        cfg.placeAllAtOnce = true;
        cfg.hpwl_scale_x = 1;
        cfg.hpwl_scale_y = 2;
        cfg.spread_scale_x = 2;
        cfg.spread_scale_y = 1;
        cfg.netShareWeight = 0.2;
        cfg.solverTolerance = 0.6e-6;
        cfg.cellGroups.emplace_back();
        cfg.cellGroups.back().insert(id_SLICE_LUTX);
        cfg.cellGroups.back().insert(id_SLICE_FFX);
        cfg.cellGroups.back().insert(id_CARRY8);
        if (!placer_heap(getCtx(), cfg))
            return false;
    } else if (placer == "sa") {
        if (!placer1(getCtx(), Placer1Cfg(getCtx())))
            return false;
    } else {
        log_error("US+ architecture does not support placer '%s'\n", placer.c_str());
    }
    fixupPlacement();
    getCtx()->attrs[getCtx()->id("step")] = std::string("place");
    archInfoToAttributes();
    return true;
}

void Arch::routeVcc()
{
    log_info("Routing Vcc connections...\n");
    // Special pass for faster routing of Vcc psuedo-net
    NetInfo *vcc = nets[id("$PACKER_VCC_NET")].get();
    bindWire(getCtx()->getNetinfoSourceWire(vcc), vcc, STRENGTH_STRONG);
#if 0
    WireId wire0 = getCtx()->getNetinfoSourceWire(vcc);
    Loc drvloc = getBelLocation(vcc->driver.cell->bel);
    BelId bel = vcc->driver.cell->bel;
    log_info("%d %d %d %d\n", vcc->driver.cell->bel.tile, drvloc.x, drvloc.y, (getBelType(bel) == id_PSEUDO_GND || getBelType(bel) == id_PSEUDO_VCC));
    log_info("%s\n", nameOfWire(wire0));
    for (auto pip1 : getPipsDownhill(wire0)) {
        WireId wire1 = getPipDstWire(pip1);
        log_info("   -> %s\n", nameOfWire(wire1));
        for (auto pip2 : getPipsDownhill(wire1))
            log_info("       -> %s\n", nameOfWire(getPipDstWire(pip2)));

    }
#endif
    for (auto &usr : vcc->users) {
        std::queue<WireId> visit;
        std::unordered_map<WireId, PipId> backtrace;
        WireId dest = WireId();
        WireId sink = getCtx()->getNetinfoSinkWire(vcc, usr);
        if (sink == WireId())
            log_error("Pin '%s' of bel '%s' has no associated wire\n", usr.port.c_str(this), nameOfBel(usr.cell->bel));
        visit.push(sink);
        while (!visit.empty()) {
            WireId curr = visit.front();
            visit.pop();
            if (getBoundWireNet(curr) == vcc) {
                dest = curr;
                break;
            }
            for (auto uh : getPipsUphill(curr)) {
                if (!checkPipAvail(uh))
                    continue;
                WireId src = getPipSrcWire(uh);
                if (backtrace.count(src))
                    continue;
                if (!checkWireAvail(src) && getBoundWireNet(src) != vcc)
                    continue;
                backtrace[src] = uh;
                visit.push(src);
            }
        }
        NPNR_ASSERT(dest != WireId());
        while (backtrace.count(dest)) {
            auto uh = backtrace[dest];
            dest = getPipDstWire(uh);
            bindWire(dest, vcc, STRENGTH_STRONG);
            bindPip(uh, vcc, STRENGTH_STRONG);
        }
    }
}

void Arch::routeClock()
{
    log_info("Routing global clocks...\n");
    // Special pass for faster routing of global clock psuedo-net
    for (auto net : sorted(nets)) {
        NetInfo *ni = net.second;
        if (ni->driver.cell == nullptr)
            continue;
        bool is_global = false;
        if ((ni->driver.cell->type == id_BUFGCTRL || ni->driver.cell->type == id_BUFCE_BUFG_PS ||
             ni->driver.cell->type == id_BUFCE_BUFCE || ni->driver.cell->type == id_BUFGCE_DIV_BUFGCE_DIV) &&
            ni->driver.port == id("O"))
            is_global = true;
        else if (ni->driver.cell->type == id("PLLE2_ADV_PLLE2_ADV") && ni->users.size() == 1 &&
                 (ni->users.front().cell->type == id_BUFGCTRL || ni->users.front().cell->type == id_BUFCE_BUFCE ||
                  ni->users.front().cell->type == id_BUFGCE_DIV_BUFGCE_DIV))
            is_global = true;
        else if (ni->users.size() == 1 && ni->users.front().cell->type == id("PLLE2_ADV_PLLE2_ADV") &&
                 ni->users.front().port == id("CLKIN1"))
            is_global = true;
        if (!is_global)
            continue;
        log_info("    routing clock '%s'\n", ni->name.c_str(this));
        bindWire(getCtx()->getNetinfoSourceWire(ni), ni, STRENGTH_LOCKED);
        for (auto &usr : ni->users) {
            std::queue<WireId> visit;
            std::unordered_map<WireId, PipId> backtrace;
            WireId dest = WireId();
            if (getCtx()->debug)
                log_info("        routing arc to %s.%s (wire %s):\n", usr.cell->name.c_str(this), usr.port.c_str(this),
                         nameOfWire(getCtx()->getNetinfoSinkWire(ni, usr)));
            visit.push(getCtx()->getNetinfoSinkWire(ni, usr));
            while (!visit.empty()) {
                WireId curr = visit.front();
                visit.pop();
                if (getBoundWireNet(curr) == ni) {
                    dest = curr;
                    break;
                }
                for (auto uh : getPipsUphill(curr)) {
                    if (!checkPipAvail(uh))
                        continue;
                    WireId src = getPipSrcWire(uh);
                    if (backtrace.count(src))
                        continue;
                    int intent = wireIntent(src);
                    if (intent == ID_NODE_DOUBLE || intent == ID_NODE_HLONG || intent == ID_NODE_HQUAD ||
                        intent == ID_NODE_VLONG || intent == ID_NODE_VQUAD || intent == ID_NODE_SINGLE ||
                        intent == ID_NODE_CLE_OUTPUT || intent == ID_NODE_OPTDELAY || intent == ID_BENTQUAD ||
                        intent == ID_DOUBLE || intent == ID_HLONG || intent == ID_HQUAD || intent == ID_OPTDELAY ||
                        intent == ID_SINGLE || intent == ID_VLONG || intent == ID_VLONG12 || intent == ID_VQUAD ||
                        intent == ID_PINBOUNCE)
                        continue;
                    if (!checkWireAvail(src) && getBoundWireNet(src) != ni)
                        continue;
                    backtrace[src] = uh;
                    visit.push(src);
                }
            }
            if (dest == WireId()) {
                log_info("            failed to find a route using dedicated resources.\n");
                if (ni->users.size() == 1 && ni->users.front().cell->type == id("PLLE2_ADV_PLLE2_ADV") &&
                    ni->users.front().port == id("CLKIN1")) {
                    // Due to some missing pips, currently special case more lenient solution
                    std::queue<WireId> empty;
                    std::swap(visit, empty);
                    backtrace.clear();
                    visit.push(getCtx()->getNetinfoSinkWire(ni, usr));
                    while (!visit.empty()) {
                        WireId curr = visit.front();
                        visit.pop();
                        if (getBoundWireNet(curr) == ni) {
                            dest = curr;
                            break;
                        }
                        for (auto uh : getPipsUphill(curr)) {
                            if (!checkPipAvail(uh))
                                continue;
                            WireId src = getPipSrcWire(uh);
                            if (backtrace.count(src))
                                continue;
                            if (!checkWireAvail(src) && getBoundWireNet(src) != ni)
                                continue;
                            backtrace[src] = uh;
                            visit.push(src);
                        }
                    }
                    if (dest == WireId())
                        continue;
                } else {
                    continue;
                }
            }
            while (backtrace.count(dest)) {
                auto uh = backtrace[dest];
                dest = getPipDstWire(uh);
                if (getCtx()->debug)
                    log_info("            bind pip %s --> %s\n", nameOfPip(uh), nameOfWire(dest));
                bindWire(dest, ni, STRENGTH_LOCKED);
                bindPip(uh, ni, STRENGTH_LOCKED);
            }
        }
    }
#if 0
    for (auto net : sorted(nets)) {
        NetInfo *ni = net.second;
        for (auto &usr : ni->users) {
            if (usr.cell->type != id_BUFGCTRL || usr.port != id("I0"))
                continue;
            WireId dst = getCtx()->getNetinfoSinkWire(ni, usr);
            std::queue<WireId> visit;
            visit.push(dst);
            int i = 0;
            while(!visit.empty() && i < 5000) {
                WireId curr = visit.front();
                visit.pop();
                log("  %s\n", nameOfWire(curr));
                for (auto pip : getPipsUphill(curr)) {
                    auto &pd = locInfo(pip).pip_data[pip.index];
                    log_info("    p %s sr %s (t %d s %d sv %d)\n", nameOfPip(pip), nameOfWire(getPipSrcWire(pip)), pd.flags, pd.site, pd.site_variant);
                    if (!checkPipAvail(pip)) {
                        log("      p unavail\n");
                        continue;
                    }
                    WireId src = getPipSrcWire(pip);
                    if (!checkWireAvail(src)) {
                        log("      w unavail (%s)\n", nameOf(getBoundWireNet(src)));
                        continue;
                    }
                    log_info("     p %s s %s\n", nameOfPip(pip), nameOfWire(src));
                    visit.push(src);
                }
                ++i;
            }
        }
    }
#endif
}

void Arch::findSourceSinkLocations()
{
    // Use a backwards BFS to find the real location of sinks, on a best-effort basis
#if 1
    for (auto net : sorted(nets)) {
        NetInfo *ni = net.second;
        for (auto &usr : ni->users) {
            BelId bel = usr.cell->bel;
            if (bel == BelId() || isLogicTile(bel) || (xc7 && isBRAMTile(bel)))
                continue; // don't need to do this for logic bels, which are always next to their INT
            WireId sink = getCtx()->getNetinfoSinkWire(ni, usr);
            if (sink == WireId() || sink_locs.count(sink))
                continue;
            std::queue<WireId> visit;
            std::unordered_map<WireId, WireId> backtrace;
            int iter = 0;
            // as this is a best-effort optimisation to slightly improve routing,
            // don't spend too long with a nice low iteration limit
            const int iter_max = 500;
            visit.push(sink);
            while (!visit.empty() && iter < iter_max) {
                ++iter;
                WireId cursor = visit.front();
                visit.pop();
                if (wireInfo(cursor).site == -1) {
                    int intent = wireIntent(cursor);
                    if (intent != ID_NODE_PINFEED && intent != ID_PSEUDO_VCC && intent != ID_PSEUDO_GND &&
                        intent != ID_INTENT_DEFAULT && intent != ID_NODE_DEDICATED && intent != ID_NODE_OPTDELAY &&
                        intent != ID_PINFEED && intent != ID_INPUT) {
                        int tile = cursor.tile == -1 ? chip_info->nodes[cursor.index].tile_wires[0].tile : cursor.tile;
                        sink_locs[sink] = Loc(tile % chip_info->width, tile / chip_info->width, 0);
                        if (getCtx()->debug) {
                            log_info("%s <---- %s\n", nameOfWire(sink), nameOfWire(cursor));
                        }

                        while (backtrace.count(cursor)) {
                            cursor = backtrace.at(cursor);
                            if (!sink_locs.count(cursor)) {
                                sink_locs[cursor] = Loc(tile % chip_info->width, tile / chip_info->width, 0);
                            }
                        }

                        break;
                    }
                }
                for (auto pip : getPipsUphill(cursor)) {
                    WireId src = getPipSrcWire(pip);
                    if (!backtrace.count(src)) {
                        backtrace[src] = cursor;
                        visit.push(getPipSrcWire(pip));
                    }
                }
            }
        }

        auto &drv = ni->driver;
        if (drv.cell != nullptr) {
            BelId bel = drv.cell->bel;
            if (bel == BelId() || isLogicTile(bel))
                continue; // don't need to do this for logic bels, which are always next to their INT
            WireId source = getCtx()->getNetinfoSourceWire(ni);
            if (source == WireId() || source_locs.count(source))
                continue;
            std::queue<WireId> visit;
            std::unordered_map<WireId, WireId> backtrace;
            int iter = 0;
            // as this is a best-effort optimisation to slightly improve routing,
            // don't spend too long with a nice low iteration limit
            const int iter_max = 500;
            visit.push(source);
            while (!visit.empty() && iter < iter_max) {
                ++iter;
                WireId cursor = visit.front();
                visit.pop();
                if (wireInfo(cursor).site == -1) {
                    int intent = wireIntent(cursor);
                    if (intent != ID_NODE_PINFEED && intent != ID_PSEUDO_VCC && intent != ID_PSEUDO_GND &&
                        intent != ID_INTENT_DEFAULT && intent != ID_NODE_DEDICATED && intent != ID_NODE_OPTDELAY &&
                        intent != ID_NODE_OUTPUT && intent != ID_NODE_INT_INTERFACE) {
                        int tile = cursor.tile == -1 ? chip_info->nodes[cursor.index].tile_wires[0].tile : cursor.tile;
                        source_locs[source] = Loc(tile % chip_info->width, tile / chip_info->width, 0);
                        if (getCtx()->debug) {
                            log_info("%s ----> %s\n", nameOfWire(source), nameOfWire(cursor));
                        }

                        while (backtrace.count(cursor)) {
                            cursor = backtrace.at(cursor);
                            if (!source_locs.count(cursor)) {
                                source_locs[cursor] = Loc(tile % chip_info->width, tile / chip_info->width, 0);
                            }
                        }

                        break;
                    }
                }
                for (auto pip : getPipsDownhill(cursor)) {
                    WireId dst = getPipDstWire(pip);
                    if (!backtrace.count(dst)) {
                        backtrace[dst] = cursor;
                        visit.push(getPipDstWire(pip));
                    }
                }
            }
        }
    }
#endif
}

bool Arch::route()
{
    assign_budget(getCtx(), true);
    std::string router = str_or_default(settings, id("router"), defaultRouter);
    if (router != "router2")
        routeVcc();
    routeClock();
    findSourceSinkLocations();

    bool result;
    if (router == "router1") {
        result = router1(getCtx(), Router1Cfg(getCtx()));
    } else if (router == "router2") {
        auto cfg = Router2Cfg(getCtx());
        cfg.bb_margin_x = 4;
        cfg.bb_margin_y = 4;
        cfg.backwards_max_iter = 200;
        cfg.perf_profile = true;
        router2(getCtx(), cfg);
        result = true;
    } else {
        log_error("Xilinx architecture does not support router '%s'\n", router.c_str());
    }
    fixupRouting();
    getCtx()->settings[getCtx()->id("route")] = 1;
    archInfoToAttributes();
    return result;
}

std::string Arch::getPackagePinSite(const std::string &pin) const
{
    if (pin_to_site.empty()) {
        for (int t = 0; t < chip_info->num_tiles; t++) {
            auto &tile = chip_info->tile_insts[t];
            for (int s = 0; s < tile.num_sites; s++) {
                auto &site = tile.site_insts[s];
                if (site.pin[0] != '\0' && site.pin[0] != '.')
                    pin_to_site[site.pin.get()] = site.name.get();
            }
        }
    }
    auto site_iter = pin_to_site.find(pin);
    return site_iter != pin_to_site.end() ? site_iter->second : "";
}

std::string Arch::getBelPackagePin(BelId bel) const
{
    int s = locInfo(bel).bel_data[bel.index].site;
    NPNR_ASSERT(s != -1);
    auto &tile = chip_info->tile_insts[bel.tile];
    auto &site = tile.site_insts[s];
    return site.pin.get();
}

// -----------------------------------------------------------------------

std::vector<GraphicElement> Arch::getDecalGraphics(DecalId decal) const
{
    std::vector<GraphicElement> ret;

    const float lut6_x0 = 0.9;
    const float lut6_x1 = 0.92;
    const float lut6_y0 = 0.1;
    const float lut6_y1 = 0.16;
    const float lut5_x0 = 0.905;
    const float lut5_x1 = 0.915;
    const float lut5_y0 = 0.11;
    const float lut5_y1 = 0.15;
    const float lut_spacing = 0.1;

    const float ff1_x0 = 0.96;
    const float ff1_x1 = 0.98;
    const float ff1_y0 = 0.1;
    const float ff1_y1 = 0.12;
    const float ff2_x0 = 0.96;
    const float ff2_x1 = 0.98;
    const float ff2_y0 = 0.16;
    const float ff2_y1 = 0.18;
    const float ff_spacing = 0.1;

    if (decal.type == DecalId::TYPE_BEL) {
        auto style = decal.active ? GraphicElement::STYLE_ACTIVE : GraphicElement::STYLE_INACTIVE;
        auto bel_data = chip_info->tile_types[decal.tile_type].bel_data[decal.index];
        if (bel_data.type == ID_SLICE_LUTX) {
            int z = bel_data.z >> 4;
            if ((bel_data.z & 0xF) == BEL_5LUT) {
                ret.emplace_back(GraphicElement::TYPE_BOX, style, lut5_x0, lut5_y0 + lut_spacing * z, lut5_x1,
                                 lut5_y1 + lut_spacing * z, 1);
            } else {
                ret.emplace_back(GraphicElement::TYPE_BOX, style, lut6_x0, lut6_y0 + lut_spacing * z, lut6_x1,
                                 lut6_y1 + lut_spacing * z, 1);
            }
        } else if (bel_data.type == ID_SLICE_FFX) {
            int z = bel_data.z >> 4;
            if ((bel_data.z & 0xF) == BEL_FF2) {
                ret.emplace_back(GraphicElement::TYPE_BOX, style, ff2_x0, ff2_y0 + ff_spacing * z, ff2_x1,
                                 ff2_y1 + lut_spacing * z, 1);
            } else {
                ret.emplace_back(GraphicElement::TYPE_BOX, style, ff1_x0, ff1_y0 + ff_spacing * z, ff1_x1,
                                 ff1_y1 + ff_spacing * z, 1);
            }
        }
    }

    const float swb_x0 = 0.2;
    const float swb_x1 = 0.7;
    const float swb_y0 = 0.2;
    const float swb_y1 = 0.7;
    const float wire_len = 0.03;
    const float wire_space = 0.001;
    const float wire_margin = 0.005;

    if (decal.type == DecalId::TYPE_WIRE) {
        WireId wire;
        wire.tile = decal.tile_type;
        wire.index = decal.index;
        auto style = decal.active ? GraphicElement::STYLE_ACTIVE : GraphicElement::STYLE_INACTIVE;

        int wire_tile = (wire.tile == -1) ? 0 : wire.tile;
        int wires_per_side = int(((swb_x1 - swb_x0) - 2 * wire_margin) / wire_space);

        for (auto w : getTileWireRange(wire)) {
            auto wire_data = locInfo(w).wire_data[w.index];
            if (wire_data.site != -1)
                continue;
            int wx = (w.tile % chip_info->width) - (wire_tile % chip_info->width),
                wy = (w.tile / chip_info->width) - (wire_tile / chip_info->width);
            int side = w.index / wires_per_side;
            int offset = w.index % wires_per_side;
            if (side == 1 || side == 3) {
                float y = wy + swb_y0 + wire_margin + offset * wire_space;
                ret.emplace_back(GraphicElement::TYPE_LINE, style, wx + ((side == 3) ? swb_x1 : (swb_x0 - wire_len)), y,
                                 wx + ((side == 3) ? (swb_x1 + wire_len) : swb_x0), y, 1);
            } else if (side == 0 || side == 2) {
                float x = wx + swb_x0 + wire_margin + offset * wire_space;
                ret.emplace_back(GraphicElement::TYPE_LINE, style, x, wy + ((side == 2) ? swb_y1 : (swb_y0 - wire_len)),
                                 x, wy + ((side == 2) ? (swb_y1 + wire_len) : swb_y0), 1);
            }
        }
    }

    return ret;
}

DecalXY Arch::getBelDecal(BelId bel) const
{
    DecalXY decalxy;
    decalxy.decal.type = DecalId::TYPE_BEL;
    decalxy.decal.index = bel.index;
    decalxy.decal.tile_type = chip_info->tile_insts[bel.tile].type;
    decalxy.decal.active = getBoundBelCell(bel) != nullptr;
    decalxy.x = bel.tile % chip_info->width;
    decalxy.y = bel.tile / chip_info->width;
    return decalxy;
}

DecalXY Arch::getWireDecal(WireId wire) const
{
    DecalXY decalxy;
    decalxy.decal.type = DecalId::TYPE_WIRE;
    decalxy.decal.index = wire.index;
    decalxy.decal.tile_type = wire.tile;
    decalxy.decal.active = getBoundWireNet(wire) != nullptr;
    int wire_tile = (wire.tile == -1) ? 0 : wire.tile;
    decalxy.x = wire_tile % chip_info->width;
    decalxy.y = wire_tile / chip_info->width;
    return decalxy;
}

DecalXY Arch::getPipDecal(PipId pip) const { return {}; };

DecalXY Arch::getGroupDecal(GroupId pip) const { return {}; };

// -----------------------------------------------------------------------

bool Arch::getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort, DelayInfo &delay) const
{
    int tt_id = -1, inst_id = -1;
    if (cell->bel != BelId()) {
        tt_id = locInfo(cell->bel).timing_index;
        inst_id = locInfo(cell->bel).bel_data[cell->bel.index].timing_inst;
    }

    if (cell->type == id_SLICE_LUTX) {
        if (xc7 && inst_id != -1) {
            int z = locInfo(cell->bel).bel_data[cell->bel.index].z;
            IdString tiletype = getBelTileType(cell->bel);
            bool is_lut5 = (z & 0xF) == BEL_5LUT;
            bool is_slicem = (tiletype == id_CLBLM_L || tiletype == ID_CLBLM_R) && (z < 64);
            IdString variant = is_slicem ? (is_lut5 ? id("LUT_OR_MEM5LRAM") : id("LUT_OR_MEM6LRAM"))
                                         : (is_lut5 ? id("LUT5") : id("LUT6"));

            if (fromPort == id_CLK)
                return false;
            return xc7_cell_timing_lookup(tt_id, inst_id, variant, (is_lut5 && fromPort == id_A6) ? id_A5 : fromPort,
                                          (is_lut5 && toPort == id_O6) ? id_O5 : toPort, delay);
        }

        if (fromPort == id_A1 || fromPort == id_A2 || fromPort == id_A3 || fromPort == id_A4 || fromPort == id_A5 ||
            fromPort == id_A6) {
            if (toPort == id_O5 || toPort == id_O6) {
                delay.delay = 200; // FIXME
                return true;
            }
        }
    } else if (cell->type == id_CARRY4) {
        if (xc7 && inst_id != -1) {
            return xc7_cell_timing_lookup(tt_id, inst_id, id("CARRY4"), fromPort, toPort, delay);
        }
    } else if (cell->type == id_F7MUX || cell->type == id_F8MUX || cell->type == id_F9MUX ||
               cell->type == id("SELMUX2_1")) {
        if (xc7 && inst_id != -1) {
            return xc7_cell_timing_lookup(tt_id, inst_id, cell->type, fromPort, toPort, delay);
        }
        delay.delay = 100;
        return true;
    } else if (cell->type == id_BUFGCTRL) {
        if (fromPort == id("I0") || fromPort == id("I1"))
            if (toPort == id("O")) {
                delay.delay = 200; // FIXME
                return true;
            }
    }
    return false;
}

TimingPortClass Arch::getPortTimingClass(const CellInfo *cell, IdString port, int &clockInfoCount) const
{
    if (cell->type == id_SLICE_LUTX) {
        if (get_net_or_empty(cell, id_O5) == nullptr && get_net_or_empty(cell, id_O6) == nullptr)
            return TMG_IGNORE;
        if (port == id_A1 || port == id_A2 || port == id_A3 || port == id_A4 || port == id_A5 || port == id_A6)
            return TMG_COMB_INPUT;
        else if (port == id_O5 || port == id_O6)
            return TMG_COMB_OUTPUT;
    } else if (cell->type == id_CARRY4 && cell->bel != BelId()) {
        return cell->ports.at(port).type == PORT_OUT ? TMG_COMB_OUTPUT : TMG_COMB_INPUT;
    } else if (cell->type == id_SLICE_FFX) {
        if (port == (xc7 ? id_CK : id_CLK))
            return TMG_CLOCK_INPUT;
        else if (port == id_Q) {
            clockInfoCount = 1;
            return TMG_REGISTER_OUTPUT;
        } else {
            clockInfoCount = 1;
            return TMG_REGISTER_INPUT;
        }
    } else if (cell->type == id_F7MUX || cell->type == id_F8MUX || cell->type == id_F9MUX ||
               cell->type == id("SELMUX2_1")) {
        if (port == id_OUT)
            return TMG_COMB_OUTPUT;
        else
            return TMG_COMB_INPUT;
    } else if (cell->type == id_IOB_IBUFCTRL) {
        if (port == id("O"))
            return TMG_STARTPOINT;
    } else if (cell->type == id_IOB_OUTBUF) {
        if (port == id("I"))
            return TMG_ENDPOINT;
    } else if (cell->type == id_BUFGCTRL) {
        if (port == id("I0") || port == id("I1"))
            return TMG_COMB_INPUT;
        if (port == id("O"))
            return TMG_COMB_OUTPUT;
    }
    return TMG_IGNORE;
}

TimingClockingInfo Arch::getPortClockingInfo(const CellInfo *cell, IdString port, int index) const
{
    TimingClockingInfo info;
    info.setup = getDelayFromNS(0.1);
    info.hold = getDelayFromNS(0.1);
    info.clockToQ = getDelayFromNS(0.1);
    info.clock_port = xc7 ? id_CK : id_CLK;
    info.edge = RISING_EDGE;
    return info;
}

int Arch::getHclkForIob(BelId pad)
{
    std::string tiletype = getBelTileType(pad).str(this);
    int ioi = pad.tile;
    // Find the IOI for IOB
    if (boost::starts_with(tiletype, "LIOB"))
        ioi += 1;
    else if (boost::starts_with(tiletype, "RIOB"))
        ioi -= 1;
    else
        NPNR_ASSERT_FALSE("unknown IOB side");
    return getHclkForIoi(ioi);
}

int Arch::getHclkForIoi(int ioi)
{
    // Find a wire driven by the HCLK
    WireId ioclk0;
    auto &td = chip_info->tile_types[chip_info->tile_insts[ioi].type];
    for (int i = 0; i < td.num_wires; i++) {
        std::string name = IdString(td.wire_data[i].name).str(this);
        if (name == "IOI_IOCLK0" || name == "IOI_SING_IOCLK0") {
            ioclk0 = canonicalWireId(chip_info, ioi, i);
            break;
        }
    }
    NPNR_ASSERT(ioclk0 != WireId());
    for (auto uh : getPipsUphill(ioclk0))
        return uh.tile;
    NPNR_ASSERT_FALSE("failed to find HCLK pips");
}
namespace {
template <typename Tres, typename Tgetter, typename Tkey>
boost::optional<const Tres &> db_binary_search(const Tres *list, int count, Tgetter key_getter, Tkey key)
{
    if (count < 7) {
        for (int i = 0; i < count; i++) {
            if (key_getter(list[i]) == key) {
                return boost::optional<const Tres &>(list[i]);
            }
        }
    } else {
        int b = 0, e = count - 1;
        while (b <= e) {
            int i = (b + e) / 2;
            if (key_getter(list[i]) == key) {
                return boost::optional<const Tres &>(list[i]);
            }
            if (key_getter(list[i]) > key)
                e = i - 1;
            else
                b = i + 1;
        }
    }
    return {};
}
} // namespace

bool Arch::xc7_cell_timing_lookup(int tt_id, int inst_id, IdString variant, IdString from_port, IdString to_port,
                                  DelayInfo &delay) const
{
    if (tt_id == -1 || inst_id == -1)
        return false;
    const InstanceTimingPOD &inst = chip_info->timing_data->tile_cell_timings[tt_id].instances[inst_id];
    auto found_var = db_binary_search(
            inst.celltypes.get(), inst.num_celltypes, [](const CellTimingPOD &ct) { return ct.variant_name; },
            variant.index);
    if (!found_var)
        return false;

    const CellTimingPOD &ct = *found_var;
    auto found_delay = db_binary_search(
            ct.delays.get(), ct.num_delays,
            [](const CellPropDelayPOD &ct) { return std::make_pair(ct.to_port, ct.from_port); },
            std::make_pair(to_port.index, from_port.index));
    if (!found_delay)
        return false;
    delay.delay = found_delay->max_delay;
    return true;
}

#ifdef WITH_HEAP
const std::string Arch::defaultPlacer = "heap";
#else
const std::string Arch::defaultPlacer = "sa";
#endif

const std::vector<std::string> Arch::availablePlacers = {"sa",
#ifdef WITH_HEAP
                                                         "heap"
#endif
};

const std::string Arch::defaultRouter = "router2";
const std::vector<std::string> Arch::availableRouters = {"router1", "router2"};

NEXTPNR_NAMESPACE_END
