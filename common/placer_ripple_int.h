/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019-2020  David Shah <dave@ds0.me>
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
#ifndef PLACER_RIPPLE_INT_H
#define PLACER_RIPPLE_INT_H
#include <queue>
#include "nextpnr.h"
#include "placer_ripple_util.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Ripple {

struct Bounds
{
    int x0, x1, y0, y1;
};

// We create our own simple netlist structures, to account for packing and other transformations as well as efficient
// handling of macros like carry chains as a single block
struct RippleCellPort
{
    std::vector<PortRef> orig_ports;
    PortType dir;
    NetInfo *net;
};
struct RippleCell
{
    int index;
    indexed_store<CellInfo *> base_cells;
    IdString type;
    bool is_macro, is_packed;
    Bounds macro_extent;
    std::vector<RippleCellPort> ext_ports;

    int area;
    int area_scale;
    int chiplet;
    double solver_x, solver_y;
    int placed_x, placed_y;
    BelId root_bel;
};

struct RippleCellIndex
{
    int cell, subcell;
};

struct Chiplet
{
    std::string name;
    int x0, y0, x1, y1;
};

struct PackedCellStructure
{
    IdString type_name;
    IdString root_bel_type;
    std::vector<IdString> celltypes_by_z;
    std::vector<int> int_z_to_bel_z_offset;
};

struct SiteLocation
{
    int x, y;
    int root_bel_z;
};

struct DeviceInfo
{
    int width, height;
    std::vector<Chiplet> chiplets;
    bool pack_bles, pack_clbs;
    PackedCellStructure ble_structure;
    PackedCellStructure clb_structure;

    std::vector<SiteLocation> bles;
    std::vector<SiteLocation> clbs;
};

class ArchFunctions
{
  public:
    virtual DeviceInfo getDeviceInfo() = 0;
    virtual double getCellArea(const CellInfo *cell) = 0;
    virtual double getBelArea(BelId bel) = 0;
    virtual bool checkBleCompatability(const std::vector<CellInfo *> &cells) = 0;
    virtual bool checkClbCompatability(const std::vector<CellInfo *> &cells) = 0;
    virtual ~ArchFunctions(){};
};

struct RippleFPGAPlacer
{
  public:
    RippleFPGAPlacer(Context *ctx, ArchFunctions *f) : ctx(ctx), f(f), d(f->getDeviceInfo()){};

  private:
    Context *ctx;
    ArchFunctions *f;
    DeviceInfo d;
    // Possibly-packed cell storage
    indexed_store<RippleCell> cells;
    // Cell udata -> ripplecell index
    std::vector<RippleCellIndex> udata_to_cell;
};

} // namespace Ripple

NEXTPNR_NAMESPACE_END

#endif