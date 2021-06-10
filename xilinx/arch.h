/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@symbioticeda.com>
 *  Copyright (C) 2018-19  David Shah <david@symbioticeda.com>
 *
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

#ifndef NEXTPNR_H
#error Include "arch.h" via "nextpnr.h" only.
#endif

#include <boost/iostreams/device/mapped_file.hpp>

#include <iostream>

NEXTPNR_NAMESPACE_BEGIN

/**** Everything in this section must be kept in sync with chipdb.py ****/

template <typename T> struct RelPtr
{
    int32_t offset;

    // void set(const T *ptr) {
    //     offset = reinterpret_cast<const char*>(ptr) -
    //              reinterpret_cast<const char*>(this);
    // }

    const T *get() const
    {
        return reinterpret_cast<const T *>(reinterpret_cast<const char *>(this) + int64_t(offset) * 4);
    }

    const T &operator[](size_t index) const { return get()[index]; }

    const T &operator*() const { return *(get()); }

    const T *operator->() const { return get(); }
};

// Quasi-deduplicated architecture
//
// Stored once per tile type (tile instance is pointer to type)
//  - Pips (referencing tile wire indices)
//  - Bels
//  - Tile-local wires
//
// Stored once per tile instance
//  - Tile wire info
//    - pointer to node for nodal wires
//
// Stored once per unique "node variety"
//  - Pips uphill and downhill (rel coordinate, tile wire index)
//  - Bel ports uphill and downhill (rel coordinate, index in tile, port name constid)
//
// Stored once per node instance
//  - Variety index
//  - List of tile wires
//  - Type information

NPNR_PACKED_STRUCT(struct BelWirePOD {
    int32_t port;       // port name constid
    int32_t type;       // port type (IN/OUT/BIDIR)
    int32_t wire_index; // connected wire index in tile, or -1 if NA
});

// LSnibble of Z: function
// MSnibble of Z: index in CLB (A-H)
enum LogicBelTypeZ
{
    BEL_6LUT = 0x0,
    BEL_5LUT = 0x1,
    BEL_FF = 0x2,
    BEL_FF2 = 0x3,
    BEL_FFMUX1 = 0x4,
    BEL_FFMUX2 = 0x5,
    BEL_OUTMUX = 0x6,
    BEL_F7MUX = 0x7,
    BEL_F8MUX = 0x8,
    BEL_F9MUX = 0x9,
    BEL_CARRY8 = 0xA,
    BEL_CLKINV = 0xB,
    BEL_RSTINV = 0xC,
    BEL_HARD0 = 0xD,
    BEL_CARRY4 = 0xF
};

enum BRAMBelTypeZ
{
    BEL_RAMFIFO36 = 0,
    BEL_RAM36 = 1,
    BEL_FIFO36 = 2,

    BEL_RAM18_U = 5,

    BEL_RAMFIFO18_L = 8,
    BEL_RAM18_L = 9,
    BEL_FIFO18_L = 10,
};

enum DSP48E2BelTypeZ
{
    BEL_DSP_PREADD_DATA = 0,
    BEL_DSP_PREADD = 1,
    BEL_DSP_A_B_DATA = 2,
    BEL_DSP_MULTIPLIER = 3,
    BEL_DSP_C_DATA = 4,
    BEL_DSP_M_DATA = 5,
    BEL_DSP_ALU = 6,
    BEL_DSP_OUTPUT = 7
};

NPNR_PACKED_STRUCT(struct BelInfoPOD {
    int32_t name;        // bel name (in site) constid
    int32_t type;        // compatible type name constid
    int32_t xl_type;     // xilinx type name constid
    int32_t timing_inst; // timing instance index in tile

    int32_t num_bel_wires;
    RelPtr<BelWirePOD> bel_wires;
    int16_t z;
    int16_t site;
    int16_t site_variant; // some sites have alternative types
    int16_t is_routing;
});

NPNR_PACKED_STRUCT(struct BelPortPOD {
    int32_t bel_index;
    int32_t port;
});

enum PipType
{
    PIP_TILE_ROUTING = 0,
    PIP_SITE_ENTRY = 1,
    PIP_SITE_EXIT = 2,
    PIP_SITE_INTERNAL = 3,
    PIP_LUT_PERMUTATION = 4,
    PIP_LUT_ROUTETHRU = 5,
    PIP_CONST_DRIVER = 6,
};

NPNR_PACKED_STRUCT(struct PipInfoPOD {
    int32_t src_index, dst_index;
    int32_t timing_class;
    int16_t padding;
    int16_t flags;

    int32_t bel;          // name of bel containing pip
    int32_t extra_data;   // for special pips like lut permutation
    int16_t site;         // site index in tile
    int16_t site_variant; // site variant index in tile
});

NPNR_PACKED_STRUCT(struct TileWireInfoPOD {
    int32_t name;
    int32_t num_uphill, num_downhill;
    int32_t timing_class;
    // Pip index inside tile
    RelPtr<int32_t> pips_uphill, pips_downhill;
    // Bel index inside tile
    int32_t num_bel_pins;
    RelPtr<BelPortPOD> bel_pins;

    int16_t site; // Site index in tile
    int16_t padding;

    int32_t intent; // xilinx intent constid
});

NPNR_PACKED_STRUCT(struct TileWireRefPOD {
    int32_t tile;
    int32_t index;
});

NPNR_PACKED_STRUCT(struct NodeInfoPOD {
    int32_t num_tile_wires;
    int32_t intent;
    RelPtr<TileWireRefPOD> tile_wires;
});

NPNR_PACKED_STRUCT(struct TileTypeInfoPOD {
    int32_t type;

    int32_t num_bels;
    RelPtr<BelInfoPOD> bel_data;

    int32_t num_wires;
    RelPtr<TileWireInfoPOD> wire_data;

    int32_t num_pips;
    RelPtr<PipInfoPOD> pip_data;

    // Cell timing data index
    int32_t timing_index;
});

NPNR_PACKED_STRUCT(struct SiteInstInfoPOD {
    RelPtr<char> name;
    RelPtr<char> pin;
    int32_t site_x, site_y;
    int32_t rel_x, rel_y;
    int32_t inter_x, inter_y;
});

NPNR_PACKED_STRUCT(struct TileInstInfoPOD {
    RelPtr<char> name;
    int32_t type;
    // Number of tile wires; excluding any site-internal wires
    // which come after general wires and are not stored here
    // as they will never be nodal
    int32_t num_tile_wires;
    // -1 if a tile-local wire; node index if nodal wire
    RelPtr<int32_t> tile_wire_to_node;

    // Site names must be per tile instance,
    // at least for now, due to differing coordinate systems
    int32_t num_sites;
    RelPtr<SiteInstInfoPOD> site_insts;
});

NPNR_PACKED_STRUCT(struct ConstIDDataPOD {
    int32_t known_id_count;
    int32_t bba_id_count;
    RelPtr<RelPtr<char>> bba_ids;
});

NPNR_PACKED_STRUCT(struct CellPropDelayPOD {
    int32_t from_port;
    int32_t to_port;
    int32_t min_delay;
    int32_t max_delay;
});

enum TimingCheckType : int32_t
{
    TIMING_CHECK_SETUP = 0,
    TIMING_CHECK_HOLD = 1,
    TIMING_CHECK_WIDTH = 2,
};

NPNR_PACKED_STRUCT(struct CellTimingCheckPOD {
    int32_t check_type;
    int32_t sig_port;
    int32_t clock_port;
    int32_t min_value;
    int32_t max_value;
});

NPNR_PACKED_STRUCT(struct CellTimingPOD {
    int32_t variant_name;
    int32_t num_delays, num_checks;
    RelPtr<CellPropDelayPOD> delays;
    RelPtr<CellTimingCheckPOD> checks;
});

NPNR_PACKED_STRUCT(struct InstanceTimingPOD {
    // Variants, sorted by name IdString
    int32_t inst_name;
    int32_t num_celltypes;
    RelPtr<CellTimingPOD> celltypes;
});

NPNR_PACKED_STRUCT(struct TileCellTimingPOD {
    int32_t tile_type_name;
    // Instances, sorted by name IdString
    int32_t num_instances;
    RelPtr<InstanceTimingPOD> instances;
});

/*
delay in ps
R in mOhm
C in fF
*/

NPNR_PACKED_STRUCT(struct WireTimingPOD { int32_t resistance, capacitance; });

NPNR_PACKED_STRUCT(struct PipTimingPOD {
    int16_t is_buffered;
    int16_t padding;
    int32_t min_delay, max_delay;
    int32_t resistance, capacitance;
});

NPNR_PACKED_STRUCT(struct TimingDataPOD {
    int32_t num_tile_types, num_wire_classes, num_pip_classes;
    RelPtr<TileCellTimingPOD> tile_cell_timings;
    RelPtr<WireTimingPOD> wire_timing_classes;
    RelPtr<PipTimingPOD> pip_timing_classes;
});

NPNR_PACKED_STRUCT(struct ChipInfoPOD {
    RelPtr<char> name;
    RelPtr<char> generator;

    int32_t version;
    int32_t width, height;
    int32_t num_tiles, num_tiletypes, num_nodes;
    RelPtr<TileTypeInfoPOD> tile_types;
    RelPtr<TileInstInfoPOD> tile_insts;
    RelPtr<NodeInfoPOD> nodes;

    RelPtr<ConstIDDataPOD> extra_constids;

    int32_t num_speed_grades;
    RelPtr<TimingDataPOD> timing_data;
});

/************************ End of chipdb section. ************************/

struct BelIterator
{
    const ChipInfoPOD *chip;
    int cursor_index;
    int cursor_tile;

    BelIterator operator++()
    {
        cursor_index++;
        while (cursor_tile < chip->num_tiles &&
               cursor_index >= chip->tile_types[chip->tile_insts[cursor_tile].type].num_bels) {
            cursor_index = 0;
            cursor_tile++;
        }
        return *this;
    }
    BelIterator operator++(int)
    {
        BelIterator prior(*this);
        ++(*this);
        return prior;
    }

    bool operator!=(const BelIterator &other) const
    {
        return cursor_index != other.cursor_index || cursor_tile != other.cursor_tile;
    }

    bool operator==(const BelIterator &other) const
    {
        return cursor_index == other.cursor_index && cursor_tile == other.cursor_tile;
    }

    BelId operator*() const
    {
        BelId ret;
        ret.tile = cursor_tile;
        ret.index = cursor_index;
        return ret;
    }
};

struct BelRange
{
    BelIterator b, e;
    BelIterator begin() const { return b; }
    BelIterator end() const { return e; }
};

// -----------------------------------------------------------------------

// Iterate over TileWires for a wire (will be more than one if nodal)
struct TileWireIterator
{
    const ChipInfoPOD *chip;
    WireId baseWire;
    int cursor = -1;

    void operator++() { cursor++; }
    bool operator!=(const TileWireIterator &other) const { return cursor != other.cursor; }

    // Returns a *denormalised* identifier always pointing to a tile wire rather than a node
    WireId operator*() const
    {
        if (baseWire.tile == -1) {
            WireId tw;
            const auto &node_wire = chip->nodes[baseWire.index].tile_wires[cursor];
            tw.tile = node_wire.tile;
            tw.index = node_wire.index;
            return tw;
        } else {
            return baseWire;
        }
    }
};

struct TileWireRange
{
    TileWireIterator b, e;
    TileWireIterator begin() const { return b; }
    TileWireIterator end() const { return e; }
};

inline WireId canonicalWireId(const ChipInfoPOD *chip_info, int32_t tile, int32_t wire)
{
    WireId id;

    if (wire >= chip_info->tile_insts[tile].num_tile_wires) {
        // Cannot be a nodal wire
        id.tile = tile;
        id.index = wire;
    } else {
        int32_t node = chip_info->tile_insts[tile].tile_wire_to_node[wire];
        if (node == -1) {
            // Not a nodal wire
            id.tile = tile;
            id.index = wire;
        } else {
            // Is a nodal wire, set tile to -1
            id.tile = -1;
            id.index = node;
        }
    }

    return id;
}

// -----------------------------------------------------------------------

struct WireIterator
{
    const ChipInfoPOD *chip;
    int cursor_index = 0;
    int cursor_tile = -1;

    WireIterator operator++()
    {
        // Iterate over nodes first, then tile wires that aren't nodes
        do {
            cursor_index++;
            if (cursor_tile == -1 && cursor_index >= chip->num_nodes) {
                cursor_tile = 0;
                cursor_index = 0;
            }
            while (cursor_tile != -1 && cursor_tile < chip->num_tiles &&
                   cursor_index >= chip->tile_types[chip->tile_insts[cursor_tile].type].num_wires) {
                cursor_index = 0;
                cursor_tile++;
            }

        } while ((cursor_tile != -1 && cursor_tile < chip->num_tiles &&
                  cursor_index < chip->tile_insts[cursor_tile].num_tile_wires &&
                  chip->tile_insts[cursor_tile].tile_wire_to_node[cursor_index] != -1));

        return *this;
    }
    WireIterator operator++(int)
    {
        WireIterator prior(*this);
        ++(*this);
        return prior;
    }

    bool operator!=(const WireIterator &other) const
    {
        return cursor_index != other.cursor_index || cursor_tile != other.cursor_tile;
    }

    bool operator==(const WireIterator &other) const
    {
        return cursor_index == other.cursor_index && cursor_tile == other.cursor_tile;
    }

    WireId operator*() const
    {
        WireId ret;
        ret.tile = cursor_tile;
        ret.index = cursor_index;
        return ret;
    }
};

struct WireRange
{
    WireIterator b, e;
    WireIterator begin() const { return b; }
    WireIterator end() const { return e; }
};

// -----------------------------------------------------------------------
struct AllPipIterator
{
    const ChipInfoPOD *chip;
    int cursor_index;
    int cursor_tile;

    AllPipIterator operator++()
    {
        cursor_index++;
        while (cursor_tile < chip->num_tiles &&
               cursor_index >= chip->tile_types[chip->tile_insts[cursor_tile].type].num_pips) {
            cursor_index = 0;
            cursor_tile++;
        }
        return *this;
    }
    AllPipIterator operator++(int)
    {
        AllPipIterator prior(*this);
        ++(*this);
        return prior;
    }

    bool operator!=(const AllPipIterator &other) const
    {
        return cursor_index != other.cursor_index || cursor_tile != other.cursor_tile;
    }

    bool operator==(const AllPipIterator &other) const
    {
        return cursor_index == other.cursor_index && cursor_tile == other.cursor_tile;
    }

    PipId operator*() const
    {
        PipId ret;
        ret.tile = cursor_tile;
        ret.index = cursor_index;
        return ret;
    }
};

struct AllPipRange
{
    AllPipIterator b, e;
    AllPipIterator begin() const { return b; }
    AllPipIterator end() const { return e; }
};

// -----------------------------------------------------------------------

struct UphillPipIterator
{
    const ChipInfoPOD *chip;
    TileWireIterator twi, twi_end;
    int cursor = -1;

    void operator++()
    {
        cursor++;
        while (true) {
            if (!(twi != twi_end))
                break;
            WireId w = *twi;
            auto &tile = chip->tile_types[chip->tile_insts[w.tile].type];
            if (cursor < tile.wire_data[w.index].num_uphill)
                break;
            ++twi;
            cursor = 0;
        }
    }
    bool operator!=(const UphillPipIterator &other) const { return twi != other.twi || cursor != other.cursor; }

    PipId operator*() const
    {
        PipId ret;
        WireId w = *twi;
        ret.tile = w.tile;
        ret.index = chip->tile_types[chip->tile_insts[w.tile].type].wire_data[w.index].pips_uphill[cursor];
        return ret;
    }
};

struct UphillPipRange
{
    UphillPipIterator b, e;
    UphillPipIterator begin() const { return b; }
    UphillPipIterator end() const { return e; }
};

struct DownhillPipIterator
{
    const ChipInfoPOD *chip;
    TileWireIterator twi, twi_end;
    int cursor = -1;

    void operator++()
    {
        cursor++;
        while (true) {
            if (!(twi != twi_end))
                break;
            WireId w = *twi;
            auto &tile = chip->tile_types[chip->tile_insts[w.tile].type];
            if (cursor < tile.wire_data[w.index].num_downhill)
                break;
            ++twi;
            cursor = 0;
        }
    }
    bool operator!=(const DownhillPipIterator &other) const { return twi != other.twi || cursor != other.cursor; }

    PipId operator*() const
    {
        PipId ret;
        WireId w = *twi;
        ret.tile = w.tile;
        ret.index = chip->tile_types[chip->tile_insts[w.tile].type].wire_data[w.index].pips_downhill[cursor];
        return ret;
    }
};

struct DownhillPipRange
{
    DownhillPipIterator b, e;
    DownhillPipIterator begin() const { return b; }
    DownhillPipIterator end() const { return e; }
};

struct BelPinIterator
{
    const ChipInfoPOD *chip;
    TileWireIterator twi, twi_end;
    int cursor = -1;

    void operator++()
    {
        cursor++;
        while (true) {
            if (!(twi != twi_end))
                break;
            WireId w = *twi;
            auto &tile = chip->tile_types[chip->tile_insts[w.tile].type];
            if (cursor < tile.wire_data[w.index].num_bel_pins)
                break;
            ++twi;
            cursor = 0;
        }
    }
    bool operator!=(const BelPinIterator &other) const { return twi != other.twi || cursor != other.cursor; }

    BelPin operator*() const
    {
        BelPin ret;
        WireId w = *twi;
        ret.bel.tile = w.tile;
        ret.bel.index = chip->tile_types[chip->tile_insts[w.tile].type].wire_data[w.index].bel_pins[cursor].bel_index;
        ret.pin.index = chip->tile_types[chip->tile_insts[w.tile].type].wire_data[w.index].bel_pins[cursor].port;
        return ret;
    }
};

struct BelPinRange
{
    BelPinIterator b, e;
    BelPinIterator begin() const { return b; }
    BelPinIterator end() const { return e; }
};

struct ArchArgs
{
    std::string chipdb;
};

struct Arch : BaseCtx
{
    boost::iostreams::mapped_file_source blob_file;
    const ChipInfoPOD *chip_info;

    mutable std::unordered_map<std::string, int> tile_by_name;
    mutable std::unordered_map<std::string, std::pair<int, int>> site_by_name;

    dict<WireId, NetInfo *> wire_to_net;
    dict<PipId, NetInfo *> pip_to_net;
    dict<WireId, std::pair<int, int>> driving_pip_loc;
    dict<WireId, NetInfo *> reserved_wires;

    struct LogicTileStatus
    {
        // z -> cell
        CellInfo *cells[128];

        // Eight-tile valid and dirty status
        struct EigthTileStatus
        {
            bool valid = true, dirty = true;
        } eights[8];
        struct HalfTileStatus
        {
            bool valid = true, dirty = true;
        } halfs[8];
    };

    struct BRAMTileStatus
    {
        CellInfo *cells[12] = {nullptr};
    };

    struct TileStatus
    {
        LogicTileStatus *lts = nullptr;
        BRAMTileStatus *bts = nullptr;
        std::vector<CellInfo *> boundcells;
        std::vector<int> sitevariant;

        ~TileStatus()
        {
            delete lts;
            delete bts;
        }
    };

    std::vector<TileStatus> tileStatus;

    ArchArgs args;
    Arch(ArchArgs args);

    bool xc7;

    std::string getChipName() const;

    IdString archId() const { return id("xilinx"); }
    ArchArgs archArgs() const { return args; }
    IdString archArgsToId(ArchArgs args) const;

    // -------------------------------------------------

    int getGridDimX() const { return chip_info->width; }
    int getGridDimY() const { return chip_info->height; }
    int getTileBelDimZ(int, int) const { return 256; }
    int getTilePipDimZ(int, int) const { return 1; }

    // -------------------------------------------------

    void setup_byname() const;

    BelId getBelByName(IdString name) const;

    IdString getBelName(BelId bel) const
    {
        NPNR_ASSERT(bel != BelId());
        int site = locInfo(bel).bel_data[bel.index].site;
        if (site != -1) {
            return id(std::string(chip_info->tile_insts[bel.tile].site_insts[site].name.get()) + "/" +
                      IdString(locInfo(bel).bel_data[bel.index].name).str(this));
        } else {
            return id(std::string(chip_info->tile_insts[bel.tile].name.get()) + "/" +
                      IdString(locInfo(bel).bel_data[bel.index].name).str(this));
        }
    }

    uint32_t getBelChecksum(BelId bel) const { return bel.index; }

    void updateLogicBel(BelId bel, CellInfo *cell)
    {
        int z = locInfo(bel).bel_data[bel.index].z;
        NPNR_ASSERT(z < 128);
        auto &tts = tileStatus[bel.tile];
        if (tts.lts == nullptr)
            tts.lts = new LogicTileStatus();
        auto &ts = *(tts.lts);
        if ((z == (((xc7 ? 3 : 7) << 4) | BEL_6LUT)) || (z == (((xc7 ? 3 : 7) << 4) | BEL_5LUT))) {
            if ((cell != nullptr && cell->lutInfo.is_memory) ||
                (ts.cells[z] != nullptr && ts.cells[z]->lutInfo.is_memory)) {
                // Special case - memory write port invalidates everything
                for (int i = 0; i < 8; i++)
                    ts.eights[i].dirty = true;
                if (xc7)
                    ts.halfs[0].dirty = true; // WCLK and CLK0 shared
            }
        }
        if ((((z & 0xF) == BEL_6LUT) || ((z & 0xF) == BEL_5LUT)) &&
            ((cell != nullptr && cell->lutInfo.is_srl) || (ts.cells[z] != nullptr && ts.cells[z]->lutInfo.is_srl))) {
            // SRLs invalidate everything due to write clock
            for (int i = 0; i < 8; i++)
                ts.eights[i].dirty = true;
            if (xc7)
                ts.halfs[0].dirty = true; // WCLK and CLK0 shared
        }
        ts.cells[z] = cell;
        // determine which sections to mark as dirty
        switch (z & 0xF) {
        case BEL_FF:
        case BEL_FF2:
            ts.halfs[(z >> 4) / 4].dirty = true;
            if ((((z >> 4) / 4) == 0) && xc7)
                ts.eights[3].dirty = true;
        /* fall-through */
        case BEL_6LUT:
        case BEL_5LUT:
            ts.eights[z >> 4].dirty = true;
            break;
        case BEL_F7MUX:
            ts.eights[z >> 4].dirty = true;
            ts.eights[(z >> 4) + 1].dirty = true;
            break;
        case BEL_F8MUX:
            ts.eights[(z >> 4) + 1].dirty = true;
            ts.eights[(z >> 4) + 2].dirty = true;
            break;
        case BEL_F9MUX:
            ts.eights[3].dirty = true;
            ts.eights[4].dirty = true;
            break;
        case BEL_CARRY8:
            for (int i = 0; i < 8; i++)
                ts.eights[i].dirty = true;
            break;
        case BEL_CARRY4:
            for (int i = ((z >> 4) / 4) * 4; i < (((z >> 4) / 4) + 1) * 4; i++)
                ts.eights[i].dirty = true;
            break;
        }
    }

    void updateBramBel(BelId bel, CellInfo *cell)
    {
        IdString type = getBelType(bel);
        if (type != id_RAMBFIFO18E2_RAMBFIFO18E2 && type != id_RAMBFIFO36E2_RAMBFIFO36E2 &&
            type != id_RAMB18E2_RAMB18E2 && type != id_FIFO18E2_FIFO18E2 && type != id_RAMB36E2_RAMB36E2 &&
            type != id_FIFO36E2_FIFO36E2 && type != id_RAMBFIFO36E1_RAMBFIFO36E1 && type != id_RAMB36E1_RAMB36E1 &&
            type != id_RAMB18E1_RAMB18E1)
            return;
        auto &tts = tileStatus[bel.tile];
        if (tts.bts == nullptr)
            tts.bts = new BRAMTileStatus();
        int z = locInfo(bel).bel_data[bel.index].z;
        NPNR_ASSERT(z >= 0 && z < 12);
        tts.bts->cells[z] = cell;
    }

    void bindBel(BelId bel, CellInfo *cell, PlaceStrength strength)
    {
        NPNR_ASSERT(bel != BelId());
        NPNR_ASSERT(tileStatus[bel.tile].boundcells[bel.index] == nullptr);

        tileStatus[bel.tile].boundcells[bel.index] = cell;
        auto &bd = locInfo(bel).bel_data[bel.index];
        int site = bd.site;
        if (site >= 0 && site < int(tileStatus[bel.tile].sitevariant.size()))
            tileStatus[bel.tile].sitevariant.at(site) = bd.site_variant;
        cell->bel = bel;
        cell->belStrength = strength;
        refreshUiBel(bel);

        if (isLogicTile(bel))
            updateLogicBel(bel, cell);
        else if (isBRAMTile(bel))
            updateBramBel(bel, cell);
    }

    void unbindBel(BelId bel)
    {
        NPNR_ASSERT(bel != BelId());
        NPNR_ASSERT(tileStatus[bel.tile].boundcells[bel.index] != nullptr);
        tileStatus[bel.tile].boundcells[bel.index]->bel = BelId();
        tileStatus[bel.tile].boundcells[bel.index]->belStrength = STRENGTH_NONE;
        tileStatus[bel.tile].boundcells[bel.index] = nullptr;
        refreshUiBel(bel);

        if (isLogicTile(bel))
            updateLogicBel(bel, nullptr);
        else if (isBRAMTile(bel))
            updateBramBel(bel, nullptr);
    }

    bool usp_bel_hard_unavail(BelId bel) const
    {
        // if (chip_info->height > 600 && (bel.tile / chip_info->width) < 752) // constrain to SLR0
        //    return true;
        if ((getBelType(bel) == id_PSEUDO_GND || getBelType(bel) == id_PSEUDO_VCC) &&
            ((bel.tile % chip_info->width) != 0))
            return true; // PSEUDO drivers must be at x=0 to have access to the global pseudo-network
        return false;
    }

    bool checkBelAvail(BelId bel) const
    {
        if (usp_bel_hard_unavail(bel))
            return false;
        NPNR_ASSERT(bel != BelId());
        return tileStatus[bel.tile].boundcells[bel.index] == nullptr;
    }

    CellInfo *getBoundBelCell(BelId bel) const
    {
        NPNR_ASSERT(bel != BelId());
        return tileStatus[bel.tile].boundcells[bel.index];
    }

    CellInfo *getConflictingBelCell(BelId bel) const
    {
        NPNR_ASSERT(bel != BelId());
        return tileStatus[bel.tile].boundcells[bel.index];
    }

    BelRange getBels() const
    {
        BelRange range;
        range.b.cursor_tile = 0;
        range.b.cursor_index = -1;
        range.b.chip = chip_info;
        ++range.b; //-1 and then ++ deals with the case of no Bels in the first tile
        range.e.cursor_tile = chip_info->width * chip_info->height;
        range.e.cursor_index = 0;
        range.e.chip = chip_info;
        return range;
    }

    Loc getBelLocation(BelId bel) const
    {
        NPNR_ASSERT(bel != BelId());
        Loc loc;
        loc.x = bel.tile % chip_info->width;
        loc.y = bel.tile / chip_info->width;
        loc.z = locInfo(bel).bel_data[bel.index].z;
        return loc;
    }

    BelId getBelByLocation(Loc loc) const;
    BelRange getBelsByTile(int x, int y) const;

    bool getBelGlobalBuf(BelId bel) const
    {
        IdString type = getBelType(bel);
        return (type == id_BUFGCTRL) || (type == id_PSEUDO_GND) || (type == id_PSEUDO_VCC) ||
               (type == id_BUFCE_BUFG_PS) || (type == id_BUFGCE_DIV_BUFGCE_DIV) || (type == id_BUFCE_BUFCE);
    }

    bool getBelHidden(BelId bel) const { return locInfo(bel).bel_data[bel.index].is_routing; }

    IdString getBelType(BelId bel) const
    {
        NPNR_ASSERT(bel != BelId());
        return IdString(locInfo(bel).bel_data[bel.index].type);
    }

    std::vector<std::pair<IdString, std::string>> getBelAttrs(BelId bel) const;

    WireId getBelPinWire(BelId bel, IdString pin) const;
    PortType getBelPinType(BelId bel, IdString pin) const;
    std::vector<IdString> getBelPins(BelId bel) const;

    bool isBelLocked(BelId bel) const;

    // -------------------------------------------------

    mutable std::unordered_map<IdString, WireId> wire_by_name_cache;

    WireId getWireByName(IdString name) const;

    const TileWireInfoPOD &wireInfo(WireId wire) const
    {
        if (wire.tile == -1) {
            const TileWireRefPOD &wr = chip_info->nodes[wire.index].tile_wires[0];
            return chip_info->tile_types[chip_info->tile_insts[wr.tile].type].wire_data[wr.index];
        } else {
            return locInfo(wire).wire_data[wire.index];
        }
    }

    IdString getWireName(WireId wire) const
    {
        NPNR_ASSERT(wire != WireId());
        if (wire.tile != -1 && locInfo(wire).wire_data[wire.index].site != -1) {
            return id(std::string("SITEWIRE/") +
                      chip_info->tile_insts[wire.tile].site_insts[locInfo(wire).wire_data[wire.index].site].name.get() +
                      std::string("/") + IdString(locInfo(wire).wire_data[wire.index].name).str(this));
        } else {
            return id(std::string(chip_info
                                          ->tile_insts[wire.tile == -1 ? chip_info->nodes[wire.index].tile_wires[0].tile
                                                                       : wire.tile]
                                          .name.get()) +
                      "/" + IdString(wireInfo(wire).name).c_str(this));
        }
    }

    IdString getWireType(WireId wire) const;
    std::vector<std::pair<IdString, std::string>> getWireAttrs(WireId wire) const;

    uint32_t getWireChecksum(WireId wire) const { return wire.index; }

    void bindWire(WireId wire, NetInfo *net, PlaceStrength strength)
    {
        NPNR_ASSERT(wire != WireId());
        NPNR_ASSERT(wire_to_net[wire] == nullptr);
        wire_to_net[wire] = net;
        net->wires[wire].pip = PipId();
        net->wires[wire].strength = strength;
        refreshUiWire(wire);
    }

    void unbindWire(WireId wire)
    {
        NPNR_ASSERT(wire != WireId());
        NPNR_ASSERT(wire_to_net[wire] != nullptr);

        auto &net_wires = wire_to_net[wire]->wires;
        auto it = net_wires.find(wire);
        NPNR_ASSERT(it != net_wires.end());

        auto pip = it->second.pip;
        if (pip != PipId()) {
            pip_to_net[pip] = nullptr;
        }

        net_wires.erase(it);
        wire_to_net[wire] = nullptr;
        refreshUiWire(wire);
    }

    bool checkWireAvail(WireId wire) const
    {
        NPNR_ASSERT(wire != WireId());
        auto w2n = wire_to_net.find(wire);
        return w2n == wire_to_net.end() || w2n->second == nullptr;
    }

    NetInfo *getReservedWireNet(WireId wire) const
    {
        NPNR_ASSERT(wire != WireId());
        auto w2n = reserved_wires.find(wire);
        return w2n == reserved_wires.end() ? nullptr : w2n->second;
    }

    NetInfo *getBoundWireNet(WireId wire) const
    {
        NPNR_ASSERT(wire != WireId());
        auto w2n = wire_to_net.find(wire);
        return w2n == wire_to_net.end() ? nullptr : w2n->second;
    }

    WireId getConflictingWireWire(WireId wire) const { return wire; }

    NetInfo *getConflictingWireNet(WireId wire) const
    {
        NPNR_ASSERT(wire != WireId());
        auto w2n = wire_to_net.find(wire);
        return w2n == wire_to_net.end() ? nullptr : w2n->second;
    }

    DelayInfo getWireDelay(WireId wire) const
    {
        DelayInfo delay;
        delay.delay = 0;
        return delay;
    }

    TileWireRange getTileWireRange(WireId wire) const
    {
        TileWireRange range;
        range.b.chip = chip_info;
        range.b.baseWire = wire;
        range.b.cursor = -1;
        ++range.b;

        range.e.chip = chip_info;
        range.e.baseWire = wire;
        if (wire.tile == -1)
            range.e.cursor = chip_info->nodes[wire.index].num_tile_wires;
        else
            range.e.cursor = 1;
        return range;
    }

    BelPinRange getWireBelPins(WireId wire) const
    {
        BelPinRange range;
        NPNR_ASSERT(wire != WireId());
        TileWireRange twr = getTileWireRange(wire);
        range.b.chip = chip_info;
        range.b.twi = twr.b;
        range.b.twi_end = twr.e;
        range.b.cursor = -1;
        ++range.b;
        range.e.chip = chip_info;
        range.e.twi = twr.e;
        range.e.twi_end = twr.e;
        range.e.cursor = 0;
        return range;
    }

    WireRange getWires() const
    {
        WireRange range;
        range.b.chip = chip_info;
        range.b.cursor_tile = -1;
        range.b.cursor_index = 0;
        range.e.chip = chip_info;
        range.e.cursor_tile = chip_info->num_tiles;
        range.e.cursor_index = 0;
        return range;
    }

    // -------------------------------------------------

    mutable std::unordered_map<IdString, PipId> pip_by_name_cache;

    PipId getPipByName(IdString name) const;

    void bindPip(PipId pip, NetInfo *net, PlaceStrength strength)
    {
        NPNR_ASSERT(pip != PipId());
        NPNR_ASSERT(pip_to_net[pip] == nullptr);

        WireId dst = canonicalWireId(chip_info, pip.tile, locInfo(pip).pip_data[pip.index].dst_index);
        NPNR_ASSERT(wire_to_net[dst] == nullptr || wire_to_net[dst] == net);

        pip_to_net[pip] = net;
        driving_pip_loc[dst] = std::make_pair(pip.tile % chip_info->width, pip.tile / chip_info->width);

        wire_to_net[dst] = net;
        net->wires[dst].pip = pip;
        net->wires[dst].strength = strength;
        refreshUiPip(pip);
        refreshUiWire(dst);
    }

    void unbindPip(PipId pip)
    {
        NPNR_ASSERT(pip != PipId());
        NPNR_ASSERT(pip_to_net[pip] != nullptr);

        WireId dst = canonicalWireId(chip_info, pip.tile, locInfo(pip).pip_data[pip.index].dst_index);
        NPNR_ASSERT(wire_to_net[dst] != nullptr);
        wire_to_net[dst] = nullptr;
        pip_to_net[pip]->wires.erase(dst);

        pip_to_net[pip] = nullptr;
        refreshUiPip(pip);
        refreshUiWire(dst);
    }

    dict<int, pool<int>> blacklist_pips;
    void setup_pip_blacklist();

    bool usp_pip_hard_unavail(PipId pip) const
    {
        if (blacklist_pips.count(locInfo(pip).type) && blacklist_pips.at(locInfo(pip).type).count(pip.index))
            return true;
        if (locInfo(pip).pip_data[pip.index].flags == PIP_SITE_ENTRY) {
            WireId dst = getPipDstWire(pip);
            if (dst.tile != -1) {
                auto &wi = wireInfo(dst);
                if (wi.intent == ID_INTENT_SITE_GND) {
                    LogicTileStatus *lts = tileStatus[dst.tile].lts;
                    if (lts != nullptr && (lts->cells[BEL_5LUT] != nullptr || lts->cells[BEL_6LUT] != nullptr))
                        return true; // Ground driver only available if lowest 5LUT and 6LUT not used
                }
            }
        } else if (locInfo(pip).pip_data[pip.index].flags == PIP_CONST_DRIVER) {
            WireId dst = getPipDstWire(pip);
            LogicTileStatus *lts = tileStatus[xc7 ? dst.tile : pip.tile].lts;
            if (lts != nullptr && (lts->cells[BEL_5LUT] != nullptr || lts->cells[BEL_6LUT] != nullptr))
                return true; // Ground driver only available if lowest 5LUT and 6LUT not used
        } else if (locInfo(pip).pip_data[pip.index].flags == PIP_SITE_INTERNAL) {
            auto &pd = locInfo(pip).pip_data[pip.index];
            if (pd.bel == ID_TRIBUF)
                return true;
            if (pd.site >= 0 && pd.site <= int(tileStatus[pip.tile].sitevariant.size()))
                if (pd.site_variant > 0 && pd.site_variant != tileStatus[pip.tile].sitevariant.at(pd.site))
                    return true;
        } else if (locInfo(pip).pip_data[pip.index].flags == PIP_LUT_PERMUTATION) {
            LogicTileStatus *lts = tileStatus[pip.tile].lts;
            if (lts == nullptr)
                return false;
            int eight = (locInfo(pip).pip_data[pip.index].extra_data >> 8) & 0xF;

            if (((locInfo(pip).pip_data[pip.index].extra_data >> 4) & 0xF) ==
                (locInfo(pip).pip_data[pip.index].extra_data & 0xF))
                return false; // from==to, always valid

            const CellInfo *lut6 = lts->cells[(eight << 4) | BEL_6LUT];
            if (lut6 != nullptr && (lut6->lutInfo.is_memory || lut6->lutInfo.is_srl))
                return true;
            const CellInfo *lut5 = lts->cells[(eight << 4) | BEL_5LUT];
            if (lut5 != nullptr && (lut5->lutInfo.is_memory || lut5->lutInfo.is_srl))
                return true;
        } else if (locInfo(pip).pip_data[pip.index].flags == PIP_LUT_ROUTETHRU) {
            int eight = (locInfo(pip).pip_data[pip.index].extra_data >> 8) & 0xF;
            int dest = (locInfo(pip).pip_data[pip.index].extra_data) & 0x1;
            if (eight == 0)
                return true; // FIXME: conflict with ground
            if (dest & 0x1)
                return true; // FIXME: routethru to MUX
            LogicTileStatus *lts = tileStatus[pip.tile].lts;
            if (lts == nullptr)
                return false;
            const CellInfo *lut6 = lts->cells[(eight << 4) | BEL_6LUT];
            if (lut6 != nullptr)
                return true;
            const CellInfo *lut5 = lts->cells[(eight << 4) | BEL_5LUT];
            if (lut5 != nullptr)
                return true;
        } /*else if (chip_info->tile_types[chip_info->tile_insts[pip.tile].type].type == ID_BRAM) {
            auto &pd = locInfo(pip).pip_data[pip.index];
            if (pd.site != -1 && pd.site_variant != 0)
                return true;
        }*/
        return false;
    }

    bool checkPipAvail(PipId pip) const
    {
        NPNR_ASSERT(pip != PipId());
        if (usp_pip_hard_unavail(pip))
            return false;
        return pip_to_net.find(pip) == pip_to_net.end() || pip_to_net.at(pip) == nullptr;
    }

    NetInfo *getBoundPipNet(PipId pip) const
    {
        NPNR_ASSERT(pip != PipId());
        auto p2n = pip_to_net.find(pip);
        return p2n == pip_to_net.end() ? nullptr : p2n->second;
    }

    WireId getConflictingPipWire(PipId pip) const
    {
        if (usp_pip_hard_unavail(pip))
            return WireId();
        return getPipDstWire(pip);
    }

    NetInfo *getConflictingPipNet(PipId pip) const
    {
        if (usp_pip_hard_unavail(pip))
            return nullptr;
        NPNR_ASSERT(pip != PipId());
        auto p2n = pip_to_net.find(pip);
        return p2n == pip_to_net.end() ? nullptr : p2n->second;
    }

    AllPipRange getPips() const
    {
        AllPipRange range;
        range.b.cursor_tile = 0;
        range.b.cursor_index = -1;
        range.b.chip = chip_info;
        ++range.b; //-1 and then ++ deals with the case of no wries in the first tile
        range.e.cursor_tile = chip_info->width * chip_info->height;
        range.e.cursor_index = 0;
        range.e.chip = chip_info;
        return range;
    }

    Loc getPipLocation(PipId pip) const
    {
        Loc loc;
        loc.x = pip.tile % chip_info->width;
        loc.y = pip.tile / chip_info->width;
        loc.z = 0;
        return loc;
    }

    IdString getPipName(PipId pip) const;

    IdString getPipType(PipId pip) const;
    std::vector<std::pair<IdString, std::string>> getPipAttrs(PipId pip) const;

    uint32_t getPipChecksum(PipId pip) const { return pip.index; }

    WireId getPipSrcWire(PipId pip) const
    {
        return canonicalWireId(chip_info, pip.tile, locInfo(pip).pip_data[pip.index].src_index);
    }

    WireId getPipDstWire(PipId pip) const
    {
        return canonicalWireId(chip_info, pip.tile, locInfo(pip).pip_data[pip.index].dst_index);
    }

    delay_t approx_pip_delay(int32_t start_intent, int32_t end_intent) const
    {
        // std::cout << IdString(start_intent).str(this) << " -> " << IdString(end_intent).str(this) << std::endl;
        if (start_intent == ID_INTENT_DEFAULT && end_intent == ID_NODE_DEDICATED)
            return 539;
        if (start_intent == ID_INTENT_DEFAULT && end_intent == ID_NODE_GLOBAL_LEAF)
            return 0;
        if (start_intent == ID_INTENT_DEFAULT && end_intent == ID_NODE_OUTPUT)
            return 530;
        if (start_intent == ID_NODE_CLE_OUTPUT && end_intent == ID_INTENT_DEFAULT)
            return 28;
        if (start_intent == ID_NODE_CLE_OUTPUT && end_intent == ID_NODE_LOCAL)
            return 77;
        if (start_intent == ID_NODE_DEDICATED && end_intent == ID_INTENT_DEFAULT)
            return 328;
        if (start_intent == ID_NODE_DOUBLE && end_intent == ID_NODE_LOCAL)
            return 0;
        if (start_intent == ID_NODE_GLOBAL_BUFG && end_intent == ID_NODE_GLOBAL_BUFG)
            return 0;
        if (start_intent == ID_NODE_GLOBAL_BUFG && end_intent == ID_NODE_GLOBAL_VDISTR)
            return 0;
        if (start_intent == ID_NODE_GLOBAL_HDISTR && end_intent == ID_INTENT_DEFAULT)
            return 0;
        if (start_intent == ID_NODE_GLOBAL_HDISTR && end_intent == ID_NODE_GLOBAL_HDISTR)
            return 324;
        if (start_intent == ID_NODE_GLOBAL_LEAF && end_intent == ID_NODE_LOCAL)
            return 0;
        if (start_intent == ID_NODE_GLOBAL_VDISTR && end_intent == ID_NODE_GLOBAL_HDISTR)
            return 0;
        if (start_intent == ID_NODE_GLOBAL_VDISTR && end_intent == ID_NODE_GLOBAL_VDISTR)
            return 6;
        if (start_intent == ID_NODE_HLONG && end_intent == ID_NODE_HLONG)
            return 57;
        if (start_intent == ID_NODE_HLONG && end_intent == ID_NODE_LOCAL)
            return 265;
        if (start_intent == ID_NODE_HLONG && end_intent == ID_NODE_VLONG)
            return 197;
        if (start_intent == ID_NODE_HQUAD && end_intent == ID_NODE_HLONG)
            return 16;
        if (start_intent == ID_NODE_HQUAD && end_intent == ID_NODE_LOCAL)
            return 115;
        if (start_intent == ID_NODE_HQUAD && end_intent == ID_NODE_VLONG)
            return 56;
        if (start_intent == ID_NODE_INT_INTERFACE && end_intent == ID_NODE_LOCAL)
            return 0;
        if (start_intent == ID_NODE_LOCAL && end_intent == ID_NODE_DOUBLE)
            return 71;
        if (start_intent == ID_NODE_LOCAL && end_intent == ID_NODE_HQUAD)
            return 0;
        if (start_intent == ID_NODE_LOCAL && end_intent == ID_NODE_LOCAL)
            return 106;
        if (start_intent == ID_NODE_LOCAL && end_intent == ID_NODE_PINBOUNCE)
            return 55;
        if (start_intent == ID_NODE_LOCAL && end_intent == ID_NODE_PINFEED)
            return 0;
        if (start_intent == ID_NODE_LOCAL && end_intent == ID_NODE_SINGLE)
            return 0;
        if (start_intent == ID_NODE_LOCAL && end_intent == ID_NODE_VQUAD)
            return 0;
        if (start_intent == ID_NODE_OUTPUT && end_intent == ID_NODE_INT_INTERFACE)
            return 0;
        if (start_intent == ID_NODE_PINBOUNCE && end_intent == ID_NODE_LOCAL)
            return 0;
        if (start_intent == ID_NODE_PINFEED && end_intent == ID_INTENT_DEFAULT)
            return 0;
        if (start_intent == ID_NODE_SINGLE && end_intent == ID_NODE_LOCAL)
            return 63;
        if (start_intent == ID_NODE_VLONG && end_intent == ID_NODE_HLONG)
            return 43;
        if (start_intent == ID_NODE_VLONG && end_intent == ID_NODE_LOCAL)
            return 159;
        if (start_intent == ID_NODE_VLONG && end_intent == ID_NODE_VLONG)
            return 94;
        if (start_intent == ID_NODE_VQUAD && end_intent == ID_NODE_HLONG)
            return 0;
        if (start_intent == ID_NODE_VQUAD && end_intent == ID_NODE_LOCAL)
            return 81;
        if (start_intent == ID_NODE_VQUAD && end_intent == ID_NODE_VLONG)
            return 0;
        return 100; // catch-all default
    }

    int32_t wireIntent(WireId wire) const
    {
        if (wire.tile == -1)
            return chip_info->nodes[wire.index].intent;
        else
            return locInfo(wire).wire_data[wire.index].intent;
    }

    DelayInfo getPipDelay(PipId pip) const
    {
        DelayInfo delay;
        NPNR_ASSERT(pip != PipId());
        if (locInfo(pip).pip_data[pip.index].flags == PIP_TILE_ROUTING) {
            int src_intent = wireIntent(getPipSrcWire(pip)), dst_intent = wireIntent(getPipDstWire(pip));
            if (src_intent == ID_NODE_GLOBAL_VDISTR || src_intent == ID_NODE_GLOBAL_HROUTE ||
                src_intent == ID_NODE_GLOBAL_VROUTE || src_intent == ID_NODE_GLOBAL_HDISTR ||
                src_intent == ID_NODE_GLOBAL_LEAF || src_intent == ID_NODE_GLOBAL_BUFG) {

                if (dst_intent == ID_NODE_LOCAL || dst_intent == ID_NODE_HLONG || dst_intent == ID_NODE_VLONG ||
                    dst_intent == ID_NODE_VQUAD || dst_intent == ID_NODE_HQUAD) {
                    // Assign a high penalty from global to local
                    delay.delay = 250;
                } else {
                    delay.delay = 100;
                }
            } else if (dst_intent == ID_NODE_LAGUNA_DATA) {
                delay.delay = 5000;
            } else {
                const delay_t pip_epsilon = 35;
                auto &pip_data = locInfo(pip).pip_data[pip.index];
                auto &pip_timing = chip_info->timing_data->pip_timing_classes[pip_data.timing_class];
                int src_len = 1;
                auto found_srcloc = driving_pip_loc.find(getPipSrcWire(pip));
                if (found_srcloc != driving_pip_loc.end()) {
                    src_len =
                            std::max(1, std::abs(found_srcloc->second.first - (pip.tile % chip_info->width)) +
                                                std::abs(found_srcloc->second.second - (pip.tile / chip_info->width)));
                }
                auto &src_timing =
                        chip_info->timing_data
                                ->wire_timing_classes[locInfo(pip).wire_data[pip_data.src_index].timing_class];
                delay_t pip_delay =
                        pip_timing.max_delay + delay_t((float(src_len * src_timing.resistance + pip_timing.resistance) *
                                                        pip_timing.capacitance) /
                                                       1e9);
                if (!pip_timing.is_buffered) {
                    auto &dst_timing =
                            chip_info->timing_data
                                    ->wire_timing_classes[locInfo(pip).wire_data[pip_data.dst_index].timing_class];
                    pip_delay += delay_t(
                            (float(src_timing.resistance + pip_timing.resistance) * dst_timing.capacitance) / 1e9);
                }
                delay.delay = std::max(pip_delay, pip_epsilon);
            }
        } else if (locInfo(pip).pip_data[pip.index].flags == PIP_LUT_ROUTETHRU) {
            delay.delay = 300;
        } else
            delay.delay = 25;
        return delay;
    }

    DownhillPipRange getPipsDownhill(WireId wire) const
    {
        DownhillPipRange range;
        NPNR_ASSERT(wire != WireId());
        TileWireRange twr = getTileWireRange(wire);
        range.b.chip = chip_info;
        range.b.twi = twr.b;
        range.b.twi_end = twr.e;
        range.b.cursor = -1;
        ++range.b;
        range.e.chip = chip_info;
        range.e.twi = twr.e;
        range.e.twi_end = twr.e;
        range.e.cursor = 0;
        return range;
    }

    UphillPipRange getPipsUphill(WireId wire) const
    {
        UphillPipRange range;
        NPNR_ASSERT(wire != WireId());
        TileWireRange twr = getTileWireRange(wire);
        range.b.chip = chip_info;
        range.b.twi = twr.b;
        range.b.twi_end = twr.e;
        range.b.cursor = -1;
        ++range.b;
        range.e.chip = chip_info;
        range.e.twi = twr.e;
        range.e.twi_end = twr.e;
        range.e.cursor = 0;
        return range;
    }

    UphillPipRange getWireAliases(WireId wire) const
    {
        UphillPipRange range;
        range.b.cursor = 0;
        range.b.twi.cursor = 0;
        range.e.cursor = 0;
        range.e.twi.cursor = 0;
        return range;
    }

    // -------------------------------------------------

    GroupId getGroupByName(IdString name) const { return GroupId(); }
    IdString getGroupName(GroupId group) const { return IdString(); }
    std::vector<GroupId> getGroups() const { return {}; }
    std::vector<BelId> getGroupBels(GroupId group) const { return {}; }
    std::vector<WireId> getGroupWires(GroupId group) const { return {}; }
    std::vector<PipId> getGroupPips(GroupId group) const { return {}; }
    std::vector<GroupId> getGroupGroups(GroupId group) const { return {}; }

    // -------------------------------------------------
    mutable IdString gnd_glbl, gnd_row, vcc_glbl, vcc_row;
    delay_t estimateDelay(WireId src, WireId dst, bool debug = false) const;
    delay_t predictDelay(const NetInfo *net_info, const PortRef &sink) const;
    ArcBounds getRouteBoundingBox(WireId src, WireId dst) const;
    delay_t getBoundingBoxCost(WireId src, WireId dst, int distance) const;
    delay_t getDelayEpsilon() const { return 20; }
    delay_t getRipupDelayPenalty() const { return 120; }
    delay_t getWireRipupDelayPenalty(WireId wire) const;
    float getDelayNS(delay_t v) const { return v * 0.001; }
    DelayInfo getDelayFromNS(float ns) const
    {
        DelayInfo del;
        del.delay = delay_t(ns * 1000);
        return del;
    }
    uint32_t getDelayChecksum(delay_t v) const { return v; }
    bool getBudgetOverride(const NetInfo *net_info, const PortRef &sink, delay_t &budget) const;

    // -------------------------------------------------

    bool pack();
    bool place();
    bool route();
    // -------------------------------------------------

    std::vector<GraphicElement> getDecalGraphics(DecalId decal) const;

    DecalXY getBelDecal(BelId bel) const;
    DecalXY getWireDecal(WireId wire) const;
    DecalXY getPipDecal(PipId pip) const;
    DecalXY getGroupDecal(GroupId group) const;

    // -------------------------------------------------

    // Get the delay through a cell from one port to another, returning false
    // if no path exists. This only considers combinational delays, as required by the Arch API
    bool getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort, DelayInfo &delay) const;
    // Get the port class, also setting clockInfoCount to the number of TimingClockingInfos associated with a port
    TimingPortClass getPortTimingClass(const CellInfo *cell, IdString port, int &clockInfoCount) const;
    // Get the TimingClockingInfo of a port
    TimingClockingInfo getPortClockingInfo(const CellInfo *cell, IdString port, int index) const;

    // -------------------------------------------------

    // Perform placement validity checks, returning false on failure (all
    // implemented in arch_place.cc)

    bool xc7_cell_timing_lookup(int tt_id, int inst_id, IdString variant, IdString from_port, IdString to_port,
                                DelayInfo &delay) const;

    // Whether or not a given cell can be placed at a given Bel
    // This is not intended for Bel type checks, but finer-grained constraints
    // such as conflicting set/reset signals, etc
    bool isValidBelForCell(CellInfo *cell, BelId bel) const;

    // Return true whether all Bels at a given location are valid
    bool isBelLocationValid(BelId bel) const;

    bool xcu_logic_tile_valid(IdString tileType, LogicTileStatus &lts) const;
    bool xc7_logic_tile_valid(IdString tileType, LogicTileStatus &lts) const;

    IdString getBelTileType(BelId bel) const { return IdString(locInfo(bel).type); }
    bool isLogicTile(BelId bel) const
    {
        IdString belTileType = getBelTileType(bel);
        return (belTileType == id_CLEL_L || belTileType == id_CLEL_R || belTileType == id_CLEM ||
                belTileType == id_CLEM_R || belTileType == id_CLBLL_L || belTileType == id_CLBLL_R ||
                belTileType == id_CLBLM_L || belTileType == id_CLBLM_R);
    }
    bool isBRAMTile(BelId bel) const
    {
        IdString belTileType = getBelTileType(bel);
        return belTileType == id_BRAM || belTileType == id_BRAM_L || belTileType == id_BRAM_R;
    }
    bool isLogicTile(WireId wire) const
    {
        if (wire.tile == -1)
            return false;
        IdString wireTileType = chip_info->tile_insts[wire.tile].type;
        return (wireTileType == id_CLEL_L || wireTileType == id_CLEL_R || wireTileType == id_CLEM ||
                wireTileType == id_CLEM_R || wireTileType == id_CLBLL_L || wireTileType == id_CLBLL_R ||
                wireTileType == id_CLBLM_L || wireTileType == id_CLBLM_R);
    }

    Loc getSiteLocInTile(BelId bel) const
    {
        Loc l;
        auto &site = chip_info->tile_insts[bel.tile].site_insts[locInfo(bel).bel_data[bel.index].site];
        l.x = site.rel_x;
        l.y = site.rel_y;
        l.z = locInfo(bel).bel_data[bel.index].site_variant;
        return l;
    }

    std::vector<std::pair<std::string, std::string>> getTilesAndTypes() const
    {
        std::vector<std::pair<std::string, std::string>> tt;
        for (int i = 0; i < chip_info->num_tiles; i++)
            tt.emplace_back(chip_info->tile_insts[i].name.get(),
                            IdString(chip_info->tile_types[chip_info->tile_insts[i].type].type).str(this));
        return tt;
    }

    // -------------------------------------------------
    // Assign architecure-specific arguments to nets and cells, which must be
    // called between packing or further
    // netlist modifications, and validity checks
    void assignArchInfo();
    void assignCellInfo(CellInfo *cell);

    void fixupPlacement();
    void fixupRouting();

    void routeVcc();
    void routeClock();
    void findSourceSinkLocations();
    std::unordered_map<WireId, Loc> sink_locs, source_locs;
    // -------------------------------------------------

    void parseXdc(std::istream &file);
    mutable std::unordered_map<std::string, std::string> pin_to_site;
    std::string getPackagePinSite(const std::string &pin) const;
    std::string getBelPackagePin(BelId bel) const;
    std::string getBelSite(BelId bel) const
    {
        int s = locInfo(bel).bel_data[bel.index].site;
        NPNR_ASSERT(s != -1);
        auto &tile = chip_info->tile_insts[bel.tile];
        auto &site = tile.site_insts[s];
        return site.name.get();
    }

    int getHclkForIob(BelId pad);
    int getHclkForIoi(int tile);

    // -------------------------------------------------

    static const std::string defaultPlacer;
    static const std::vector<std::string> availablePlacers;

    static const std::string defaultRouter;
    static const std::vector<std::string> availableRouters;

    // -------------------------------------------------
    template <typename Id> const TileTypeInfoPOD &locInfo(Id &id) const
    {
        return chip_info->tile_types[chip_info->tile_insts[id.tile].type];
    }
    // -------------------------------------------------
    void writeFasm(const std::string &filename);
};

NEXTPNR_NAMESPACE_END
