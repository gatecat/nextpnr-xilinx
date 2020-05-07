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
    int x0 = std::numeric_limits<int>::max(), x1 = std::numeric_limits<int>::min();
    int y0 = std::numeric_limits<int>::max(), y1 = std::numeric_limits<int>::min();
    void expand(int x, int y)
    {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    void expand(Loc loc) { expand(loc.x, loc.y); }
    int hpwl() const
    {
        if (x0 == std::numeric_limits<int>::max())
            return 0;
        return (x1 - x0) + (y1 - y0);
    }
};

// We create our own simple netlist structures, to account for packing and other transformations as well as efficient
// handling of macros like carry chains as a single block
struct RippleCellPort
{
    std::vector<PortRef> orig_ports;
    PortType dir;
    NetInfo *net;
};
struct RippleSubcell
{
    CellInfo *ci;
    int offset_x = 0, offset_y = 0;
};
struct RippleCell
{
    int index;
    indexed_store<RippleSubcell> base_cells;
    IdString type;
    bool is_macro, is_packed;
    Bounds macro_extent;
    std::vector<RippleCellPort> ext_ports;

    float area = 0;
    float area_scale = 1;
    int chiplet = -1;
    double solver_x, solver_y;
    int placed_x, placed_y;
    bool locked = false;

    BelId root_bel;
};

// An index allowing us to map a nextpnr cell into a possibly-packed RippleCell
// cell is the index of the RippleCell, subcell is the index into RippleCell::base_cells
struct RippleCellIndex
{
    RippleCellIndex(int cell, int subcell) : cell(cell), subcell(){};
    int cell, subcell;
};

// For devices such as UltraScale+, where the FPGA is split into "chiplets" (i.e. SLRs) on an interposer,
// and we want to avoid spreading strongly-connected logic across chiplets as interposer routing is expensive
// compared to routing within the chiplets
struct Chiplet
{
    std::string name;
    int x0, y0, x1, y1;
};

struct DeviceInfo
{
    int width, height;
    // This should be empty, rather than having one entry, if the device doesn't use chiplets
    std::vector<Chiplet> chiplets;

    // Some cells, like logic and BRAM, might be combined into a site
    // This is used when they are closely tied or in conflict so have to be placed together
    std::unordered_map<IdString, IdString> celltype_to_sitetype;
    std::unordered_map<IdString, std::vector<Loc>> site_locations;

    IdString clb_type, ble_type;
};

class ArchFunctions
{
  public:
    virtual DeviceInfo getDeviceInfo() = 0;
    virtual double getCellArea(const CellInfo *cell) = 0;
    virtual bool checkBleCompatability(const std::vector<CellInfo *> &cells) = 0;
    virtual bool checkClbCompatability(const std::vector<CellInfo *> &cells) = 0;
    virtual Loc getSwitchbox(Loc cell_loc) = 0;
    virtual ~ArchFunctions(){};
};

struct RippleFPGAPlacer
{
  public:
    RippleFPGAPlacer(Context *ctx, ArchFunctions *f)
            : ctx(ctx), f(f), d(f->getDeviceInfo()), grid(d.width, d.height, GridLocation{}){};

  private:
    Context *ctx;
    ArchFunctions *f;
    DeviceInfo d;
    // Possibly-packed cell storage
    indexed_store<RippleCell> cells;

    std::vector<CellInfo *> cells_by_udata;
    // Cell udata -> ripplecell index
    std::vector<RippleCellIndex> cell_index;

    // Nets that consume no routing resources because they have been "swallowed" during BLE packing
    std::unordered_set<IdString> packed_nets;

    void init_cells();
    void process_chain(int root, CellInfo *ci, int dx, int dy);

    void place_constraints();
    void place_initial();
    void partition();
    void place_global(int stage);
    void pack_ble();
    void pack_clb();
    void place_detail();

    void lower_bound_solver(double tol, double alpha, int iters);
    void upper_bound_spread();

    void run();

    void est_congestion_map(array2d<double> &cong);
    int get_rr_cost(Loc a, Loc b);

    Loc get_cell_location(const CellInfo *cell);

    struct GridLocation
    {
        int chiplet;
        std::vector<int> placed_cells;

        struct PerType
        {
            float cell_area = 0.0;
            float avail_area = 0.0;
            float target_density = 1.0;
        };

        std::vector<PerType> per_type;

        int region_id;
        Loc switchbox;
    };

    std::vector<IdString> site_types;
    std::unordered_map<IdString, int> sitetype_to_idx;

    array2d<GridLocation> grid;

    void setup_spreader_grid();
};

} // namespace Ripple

NEXTPNR_NAMESPACE_END

#endif