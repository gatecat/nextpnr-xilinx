/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2020  David Shah <dave@ds0.me>
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

#include "placer_ripple_int.h"

#include "log.h"

NEXTPNR_NAMESPACE_BEGIN
namespace Ripple {

class RippleXilinx final : public ArchFunctions
{
  public:
    RippleXilinx(Context *ctx) : ctx(ctx){};
    void setPlacer(RippleFPGAPlacer *placer) override { this->placer = placer; }
    DeviceInfo getDeviceInfo() override;
    double getCellArea(const CellInfo *cell) override;
    Loc getSwitchbox(Loc cell_loc) override;
    void doBlePacking() override;

  private:
    Context *ctx;
    RippleFPGAPlacer *placer;

    // Structures and functions for BLE packing
    std::unordered_map<IdString, IdString> lut2ff;
    void find_lut_ffs();
    void find_lut_luts(){};
};

DeviceInfo RippleXilinx::getDeviceInfo()
{
    DeviceInfo info;
    info.width = ctx->chip_info->width;
    info.height = ctx->chip_info->height;
    // FIXME: SLR regions, once they are in the database
    IdString slice = ctx->id("SLICE"), bram = ctx->id("BRAM");
    // Consider all logic elements as part of a SLICE
    info.celltype_to_sitetype[id_SLICE_LUTX] = slice;
    info.celltype_to_sitetype[id_SLICE_FFX] = slice;
    info.celltype_to_sitetype[id_CARRY4] = slice;
    info.celltype_to_sitetype[id_CARRY8] = slice;
    info.celltype_to_sitetype[id_F7MUX] = slice;
    info.celltype_to_sitetype[id_F8MUX] = slice;
    info.celltype_to_sitetype[id_F9MUX] = slice;
    info.celltype_to_sitetype[ctx->id("SELMUX2_1")] = slice;

    info.celltype_to_sitetype[id_RAMB18E1_RAMB18E1] = bram;
    info.celltype_to_sitetype[id_RAMB36E1_RAMB36E1] = bram;
    info.celltype_to_sitetype[id_RAMB18E2_RAMB18E2] = bram;
    info.celltype_to_sitetype[id_RAMB36E2_RAMB36E2] = bram;

    for (int y = 0; y < info.height; y++) {
        for (int x = 0; x < info.width; x++) {
            for (BelId b : ctx->getBelsByTile(x, y)) {
                if (ctx->getBelType(b) == id_SLICE_LUTX) {
                    int z = ctx->getBelLocation(b).z;
                    if ((z & 0xF) != BEL_6LUT)
                        continue;
                    info.site_locations[slice].push_back(Loc(x, y, z));
                }
                if (ctx->getBelType(b) == id_RAMB36E1_RAMB36E1 || ctx->getBelType(b) == id_RAMB36E2_RAMB36E2)
                    info.site_locations[bram].push_back(Loc(x, y, 0));
            }
        }
    }

    return info;
}

double RippleXilinx::getCellArea(const CellInfo *cell)
{
    if (cell->type == id_SLICE_LUTX) {
        // Bigger LUTs cost more 'area' than smaller ones
        return 0.5 + 0.1 * int(cell->ports.size());
    } else if (cell->type == id_SLICE_FFX) {
        // FFs can be considered cheaper than LUTs
        return 0.75;
    } else if (cell->type == id_F7MUX || cell->type == id_F8MUX || cell->type == id_F9MUX) {
        // Muxes are virtually free
        return 0.1;
    } else if (cell->type == id_CARRY4 || cell->type == id_CARRY8) {
        // As are CARRYs
        return 0.2;
    } else if (cell->type == id_RAMB36E2_RAMB36E2 || cell->type == id_RAMB36E1_RAMB36E1) {
        return 1.0;
    } else if (cell->type == id_RAMB18E1_RAMB18E1 || cell->type == id_RAMB18E2_RAMB18E2) {
        return 0.5;
    }
    return 1.0;
}

Loc RippleXilinx::getSwitchbox(Loc cell_loc)
{
    BelId bel = ctx->getBelByLocation(cell_loc);
    if (bel == BelId())
        return Loc(cell_loc.x, cell_loc.y, 0);
    int site = ctx->locInfo(bel).bel_data[bel.index].site;
    if (site == -1)
        return Loc(cell_loc.x, cell_loc.y, 0);
    auto site_data = ctx->chip_info->tile_insts[bel.tile].site_insts[site];
    if (site_data.inter_x != -1)
        return Loc(site_data.inter_x, site_data.inter_y, 0);
    else
        return Loc(cell_loc.x, cell_loc.y, 0);
}

void RippleXilinx::doBlePacking()
{
    find_lut_ffs();
    find_lut_luts();
}

void RippleXilinx::find_lut_ffs()
{
    int merged = 0;
    for (auto cell : placer->cells) {
        // Currently, only pack FFs with unconstrained LUTs
        if (GetSize(cell.base_cells) != 1)
            continue;
        CellInfo *ff = cell.base_cells[0].ci;
        if (ff->type != id_SLICE_FFX)
            continue;
        // Look at the D port of the FF for the driving LUT
        NetInfo *d = get_net_or_empty(ff, id_D);
        if (d == nullptr)
            continue;
        CellInfo *lut = d->driver.cell;
        if (lut == nullptr || lut->type != id_SLICE_LUTX)
            continue;
        auto lut_idx = placer->cell_index.at(lut->udata);
        auto &lut_cell = placer->cells.at(lut_idx.cell);
        if (GetSize(lut_cell.base_cells) != 1)
            continue;
        // Should we add a heuristic here to skip merging if distance
        // is very high??
        placer->merge_cells(lut_cell, cell, BEL_FF - BEL_6LUT);
        ++merged;
    }
    log_info("BLE packer merged %d LUT-FF pairs\n", merged);
}

} // namespace Ripple

void Arch::place_ripple()
{
    using namespace Ripple;
    RippleXilinx xlnx(getCtx());
    RippleFPGAPlacer placer(getCtx(), &xlnx);
    placer.run();
}

NEXTPNR_NAMESPACE_END
