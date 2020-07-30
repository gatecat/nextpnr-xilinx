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
#include <deque>
#include <queue>
#include "hypergraph.h"
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
// handling of macros like carry chains as a single block. A RippleCell might therefore be a single 'nextpnr' cell
// or a combination of them, packed into a 'basic logic element'
struct RippleCellPort
{
    std::vector<PortRef> orig_ports;
    PortType dir;
    NetInfo *net;
};
// A RippleSubcell represents a single, leaf nextpnr cell; that a RippleCell may comprise one or more off
struct RippleSubcell
{
    CellInfo *ci;
    // These are added to the location of the RippleCell to get the exact placement location
    // for this subcell
    int offset_x = 0, offset_y = 0, offset_z = 0;
    bool abs_z = false;
    inline Loc actual_loc(const Loc &root_loc) const
    {
        Loc result;
        result.x = root_loc.x + offset_x;
        result.y = root_loc.y + offset_y;
        result.z = abs_z ? offset_z : (root_loc.z + offset_z);
        return result;
    }
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

    Loc root_loc;
    bool placed = false;
};

// An index allowing us to map a nextpnr cell into a possibly-packed RippleCell
// cell is the index of the RippleCell, subcell is the index into RippleCell::base_cells
struct RippleCellIndex
{
    RippleCellIndex(int cell, int subcell) : cell(cell), subcell(subcell){};
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

    // Hints for SLICE elements. There may be a more generic way of doing this in the long term
    std::unordered_set<IdString> lut_types, dff_types, slice_misc_types;

    // IO buffer types, that are not solved by the solver but randomly placed
    // as a crude way of preventing singular matrices
    std::unordered_set<IdString> iobuf_types;
};

class ArchFunctions
{
  public:
    // Return a DeviceInfo structure for the current context
    virtual DeviceInfo getDeviceInfo() = 0;
    // Gets the area value of a score
    virtual double getCellArea(const CellInfo *cell) = 0;
    // Get the switchbox location given a cell location
    virtual Loc getSwitchbox(Loc cell_loc) = 0;
    // Do any packing of LUTs and FFs into BLEs (basic logic elements)
    // as described in the HeAP paper. Due to the large variety of arches
    // this is currently not generic.
    virtual void doBlePacking() = 0;
    virtual ~ArchFunctions(){};
};

struct RippleFPGAPlacer
{
  public:
    RippleFPGAPlacer(Context *ctx, ArchFunctions *f)
            : ctx(ctx), f(f), d(f->getDeviceInfo()), grid(d.width, d.height, GridLocation{}){};

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

    void run();

    void lower_bound_solver(double tol, double alpha, int iters);
    void upper_bound_spread();

    void est_congestion_map(array2d<double> &cong);
    int get_rr_cost(Loc a, Loc b);

    Loc get_cell_location(const CellInfo *cell);

    bool has_connectivity(RippleCell &cell);

    struct GridLocation
    {
        int chiplet;
        std::vector<int> placed_cells;

        struct PerType
        {
            double cell_area = 0.0f;
            double avail_area = 0.0f;
            double target_density = 1.0f;
            inline double target_area() const { return avail_area * target_density; }
            bool overfull = false;
        };

        std::vector<PerType> per_type;

        int region_id;
        Loc switchbox;
    };

    std::vector<IdString> site_types;
    std::unordered_map<IdString, int> sitetype_to_idx;

    array2d<GridLocation> grid;

    struct SpreaderBin
    {
        int x0, y0, x1, y1;
        double avail_area = 0.0f;
        double cell_area = 0.0f;
        double target_area = 0.0f;
        double target_density = 0.0f;
        bool is_overfull = false;
        std::vector<RippleCellIndex> placed_cells;
    };

    struct OverfilledRegion
    {
        OverfilledRegion(int x, int y) : cx(x), cy(y), x0(x), y0(y), x1(x), y1(y){};
        int cx, cy;         // centre location
        int x0, y0, x1, y1; // expanded bounds
        double target_overutil_area = 0.0;
        double strict_overutil_area = 0.0;
        double expanded_cell_area = 0.0;
        double expanded_avail_area = 0.0;
        double expanded_target_area = 0.0;

        double target_overutil_ratio = 0.0;
        double strict_overutil_ratio = 0.0;
    };

    struct SpreaderSiteType
    {
        array2d<SpreaderBin> bins;
        std::deque<OverfilledRegion> overfull;
        double avail_area = 0.0;
        double target_area = 0.0;
        double cell_area = 0.0;
    };

    struct SpreaderBox
    {
        // Cells that are rooted in the region
        std::vector<int> spread_cells;
        // Region using bin coordinates
        int x0, y0, x1, y1;
        enum Direction
        {
            HORIZ,
            VERT
        } dir;
        int level;

        SpreaderBox() : x0(-1), y0(-1), x1(-1), y1(-1), dir(HORIZ), level(0){};
        SpreaderBox(int x0, int y0, int x1, int y1, Direction dir = HORIZ, int level = 0)
                : x0(x0), y0(y0), x1(x1), y1(y1), dir(dir), level(level){};
    };

    int bin_w, bin_h;

    std::vector<SpreaderSiteType> spread_site_data;

    double expand_box_ratio = 0.0, expand_box_limit = 0.0;

    int min_expand_size = 1;

    // Data structure for representing a move made by the detail placer, and the cost changes
    // associated with it

    struct MovedCell
    {
        int cell;
        Loc old_root;
    };

    // Data structures fast incremental bounding box updates
    struct NetBoundingBox
    {
        // Actual bounding box
        int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        // Number of cells at each extremity
        int nx0 = 0, nx1 = 0, ny0 = 0, ny1 = 0;

        inline int hpwl() const { return (x1 - x0) + (y1 - y0); }
    };

    enum BoundChangeType
    {
        NO_CHANGE,
        CELL_MOVED_INWARDS,
        CELL_MOVED_OUTWARDS,
        FULL_RECOMPUTE
    };

    struct DetailArcData
    {
        bool cost_changed = false;
        float old_cost, new_cost;
    };
    struct DetailNetData
    {
        NetBoundingBox curr_bounds, new_bounds;
        BoundChangeType change_type_x = NO_CHANGE, change_type_y = NO_CHANGE;
        std::vector<DetailArcData> arcs;
    };

    struct DetailMove
    {
        std::vector<int> move_cells;

        Loc new_root_loc;

        // map conflicting cell -> new loc for that cell
        std::map<int, Loc> conflicts;

        std::vector<MovedCell> moved;

        // The list of nets and arcs that have changed bounds/delay as a result of the move
        std::vector<int> bounds_changed_nets_x, bounds_changed_nets_y;
        std::vector<std::pair<int, int>> cost_changed_arcs;

        // The current kind of change that exists for a net/arc
        std::vector<BoundChangeType> already_bounds_changed_x, already_bounds_changed_y;

        // The cost deltas
        int wirelen_delta;
        int routeability_delta;
    };

    struct HypergraphMap
    {
        std::unordered_map<int, int> cell_to_node;
        std::unordered_map<IdString, int> net_to_edge;
    };

    void export_hypergraph(HyperPart::Hypergraph &g, HypergraphMap &m);

    void setup_spreader_grid();
    void setup_spreader_bins(int bin_w, int bin_h);
    void reset_spread_cell_areas(int site_type, int x0, int y0, int x1, int y1);
    void update_spread_cell_area(int site_type, int x0, int y0, int x1, int y1);
    void find_overfilled_regions();
    void expand_overfilled_region(int st, OverfilledRegion &of);
    bool spread_cells(int site_type, SpreaderBox &box, std::vector<SpreaderBox> &spread_boxes);
    void spread_cells_in_region(int site_type, OverfilledRegion &of);
    void cut_cells_by_area(const std::vector<int> &cells_in, std::vector<int> &lo, std::vector<int> &hi, double ratio);

    int total_hpwl();

    bool place_cell(int cell, Loc root, DetailMove *move_to_update = nullptr);
    void ripup_cell(int cell);
    bool check_placement(int cell);

    // conflicts is a map: cell index --> offset from root
    bool find_conflicting_cells(int cell, Loc root, std::map<int, Loc> &conflicts);

    std::vector<DetailNetData> dt_nets;
    std::vector<NetInfo *> nets_by_udata;

    bool detail_find_candidate_locs(std::vector<int> cell, DetailMove &optimal);

    void setup_detail();
    NetBoundingBox get_net_bounds(NetInfo *net);
    bool find_move_conflicts(DetailMove &move);
    bool cost_ignore_net(NetInfo *net)
    { /* STUB */
        return false;
    }
    void update_move_costs(DetailMove &move, CellInfo *cell, BelId old_bel);
    Loc move_get_cell_loc(DetailMove &move, int i);
    void compute_move_costs(DetailMove &move);
    void reset_move(DetailMove &move);
    bool perform_move(DetailMove &move);
    void revert_move(DetailMove &move);
    void finalise_move(DetailMove &move);
};

} // namespace Ripple

NEXTPNR_NAMESPACE_END

#endif