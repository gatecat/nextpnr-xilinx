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

NEXTPNR_NAMESPACE_BEGIN
namespace Ripple {

class RippleXilinx : ArchFunctions
{
  public:
    RippleXilinx(Context *ctx) : ctx(ctx){};
    DeviceInfo getDeviceInfo() override;
    double getCellArea(const CellInfo *cell) override;
    bool checkBleCompatability(const std::vector<CellInfo *> &cells) override;
    bool checkClbCompatability(const std::vector<CellInfo *> &cells) override;
    Loc getSwitchbox(Loc cell_loc) override;

  private:
    Context *ctx;
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

} // namespace Ripple
NEXTPNR_NAMESPACE_END
