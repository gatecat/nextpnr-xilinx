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
#include <queue>
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"
#include "util.h"
NEXTPNR_NAMESPACE_BEGIN

inline NetInfo *port_or_nullptr(const CellInfo *cell, IdString name)
{
    auto found = cell->ports.find(name);
    if (found == cell->ports.end())
        return nullptr;
    return found->second.net;
}

//#define DEBUG_VALIDITY
#ifdef DEBUG_VALIDITY
#define DBG() log_info("invalid: %s %d\n", __FILE__, __LINE__)
#else
#define DBG()
#endif

bool Arch::xcu_logic_tile_valid(IdString tileType, LogicTileStatus &lts) const
{
    bool is_slicem = (tileType == id_CLEM) || (tileType == id_CLEM_R);
    bool tile_is_memory = false;
    if (lts.cells[(7 << 4) | BEL_6LUT] != nullptr && lts.cells[(7 << 4) | BEL_6LUT]->lutInfo.is_memory)
        tile_is_memory = true;
    // Check eight-tiles (mostly LUT-related validity)
    for (int i = 0; i < 8; i++) {
        if (lts.eights[i].dirty) {
            lts.eights[i].dirty = false;
            lts.eights[i].valid = false;

            CellInfo *lut6 = lts.cells[(i << 4) | BEL_6LUT];
            CellInfo *lut5 = lts.cells[(i << 4) | BEL_5LUT];

            // Check 6LUT
            if (lut6 != nullptr) {
                if (!is_slicem && (lut6->lutInfo.is_memory || lut6->lutInfo.is_srl))
                    return false; // Memory and SRLs only valid in SLICEMs
                if (lut5 != nullptr) {
                    // Can't mix memory and non-memory
                    if (lut6->lutInfo.is_memory != lut5->lutInfo.is_memory ||
                        lut6->lutInfo.is_srl != lut5->lutInfo.is_srl)
                        return false;
                    // If all 6 inputs or 2 outputs are used, 5LUT can't also be present
                    if (lut6->lutInfo.input_count == 6 || lut6->lutInfo.output_count == 2)
                        return false;
                    // If more than 5 total inputs are used, need to check number of shared input
                    if ((lut6->lutInfo.input_count + lut5->lutInfo.input_count) > 5) {
                        int shared = 0, need_shared = (lut6->lutInfo.input_count + lut5->lutInfo.input_count - 5);
                        for (int j = 0; j < lut6->lutInfo.input_count; j++) {
                            for (int k = 0; k < lut5->lutInfo.input_count; k++) {
                                if (lut6->lutInfo.input_sigs[j] == lut5->lutInfo.input_sigs[k])
                                    shared++;
                                if (shared >= need_shared)
                                    break;
                            }
                        }
                        if (shared < need_shared) {
                            DBG();
                            return false;
                        }
                    }
                }
            }
            if (lut5 != nullptr) {
                if (!is_slicem && (lut5->lutInfo.is_memory || lut5->lutInfo.is_srl)) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
                // 5LUT can use at most 5 inputs and 1 output
                if (lut5->lutInfo.input_count > 5 || lut5->lutInfo.output_count == 2) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
            }

            // Check (over)usage of DI and X inputs
            NetInfo *i_net = nullptr, *x_net = nullptr;
            if (lut6 != nullptr) {
                i_net = lut6->lutInfo.di1_net;
                x_net = lut6->lutInfo.di2_net;
            }
            if (lut5 != nullptr) {
                if (lut5->lutInfo.di1_net != nullptr) {
                    if (i_net == nullptr)
                        i_net = lut5->lutInfo.di1_net;
                    else if (i_net != lut5->lutInfo.di1_net) {
                        DBG();
                        return false; // Memory and SRLs only valid in SLICEMs
                    }
                }
                // DI2 not available for 5LUT
                if (lut5->lutInfo.di2_net != nullptr) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
            }

            CellInfo *mux = nullptr;
            // Eights A, C, E, G: F7MUX uses X input
            if (i == 0 || i == 2 || i == 4 || i == 6)
                mux = lts.cells[i << 4 | BEL_F7MUX];
            // Eights B, F: F8MUX uses X input
            if (i == 1 || i == 5)
                mux = lts.cells[(i - 1) << 4 | BEL_F8MUX];
            // Eights D: F9MUX uses X input
            if (i == 3)
                mux = lts.cells[BEL_F9MUX];

            if (mux != nullptr) {
                if (x_net == nullptr)
                    x_net = mux->muxInfo.sel;
                else if (x_net != mux->muxInfo.sel) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
            }

            CellInfo *out_fmux = nullptr;
            // Eights B, D, F, H: F7MUX connects to F7F8 out
            if (i == 1 || i == 3 || i == 5 || i == 7)
                out_fmux = lts.cells[(i - 1) << 4 | BEL_F7MUX];
            // Eights C, G: F8MUX connects to F7F8 out
            if (i == 2 || i == 6)
                out_fmux = lts.cells[(i - 2) << 4 | BEL_F8MUX];
            // Eights E: F9MUX connects to F7F8 out
            if (i == 4)
                out_fmux = lts.cells[BEL_F9MUX];

            CellInfo *carry8 = lts.cells[BEL_CARRY8];
            // CARRY8 might use X
            if (carry8 != nullptr && carry8->carryInfo.x_sigs[i] != nullptr) {
                if (x_net == nullptr)
                    x_net = carry8->carryInfo.x_sigs[i];
                else if (x_net != carry8->carryInfo.x_sigs[i]) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
            }

            // FF1 might use X, if it isn't driven directly
            CellInfo *ff1 = lts.cells[i << 4 | BEL_FF];
            if (ff1 != nullptr && ff1->ffInfo.d != nullptr && ff1->ffInfo.d->driver.cell != nullptr) {
                auto &drv = ff1->ffInfo.d->driver;
                if ((drv.cell == lut6 && drv.port != id_MC31) || drv.cell == lut5 || drv.cell == out_fmux) {
                    // Direct, OK
                    // FIXME: CARRY8 direct
                } else {
                    // Indirect, must use X input
                    if (x_net == nullptr)
                        x_net = ff1->ffInfo.d;
                    else if (x_net != ff1->ffInfo.d) {
                        DBG();
                        return false; // Memory and SRLs only valid in SLICEMs
                    }
                }
            }

            // FF2 might use I, if it isn't driven directly
            CellInfo *ff2 = lts.cells[i << 4 | BEL_FF2];
            if (ff2 != nullptr && ff2->ffInfo.d != nullptr && ff2->ffInfo.d->driver.cell != nullptr) {
                auto &drv = ff2->ffInfo.d->driver;
                if ((drv.cell == lut6 && drv.port != id_MC31) || drv.cell == lut5 || drv.cell == out_fmux) {
                    // Direct, OK
                    // FIXME: CARRY8 direct
                } else {
                    // Indirect, must use X input
                    if (i_net == nullptr)
                        i_net = ff2->ffInfo.d;
                    else if (i_net != ff2->ffInfo.d) {
                        DBG();
                        return false; // Memory and SRLs only valid in SLICEMs
                    }
                }
            }

            // Collision with top address bits
            if (tile_is_memory) {
                CellInfo *top_lut = lts.cells[(7 << 4) | BEL_6LUT];
                if (top_lut != nullptr) {
                    if ((i == 6) && x_net != top_lut->lutInfo.address_msb[0])
                        return false;
                    if ((i == 5) && x_net != top_lut->lutInfo.address_msb[1])
                        return false;
                    if ((i == 3) && x_net != top_lut->lutInfo.address_msb[2])
                        return false;
                }
            }

            bool mux_output_used = false;
            NetInfo *out5 = nullptr;
            if (lut6 != nullptr && lut6->lutInfo.output_count == 2)
                out5 = lut6->lutInfo.output_sigs[1];
            else if (lut5 != nullptr && !lut5->lutInfo.only_drives_carry)
                out5 = lut5->lutInfo.output_sigs[0];
            if (out5 != nullptr && (out5->users.size() > 1 || ((ff1 == nullptr || out5 != ff1->ffInfo.d) &&
                                                               (ff2 == nullptr || out5 != ff2->ffInfo.d)))) {
                mux_output_used = true;
            }

            if (carry8 != nullptr && carry8->carryInfo.out_sigs[i] != nullptr) {
                // FIXME: direct connections to FF
                if (mux_output_used) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
                mux_output_used = true;
            }
            if (out_fmux != nullptr) {
                NetInfo *f7f8 = out_fmux->muxInfo.out;
                if (f7f8 != nullptr && (f7f8->users.size() > 1 || ((ff1 == nullptr || f7f8 != ff1->ffInfo.d) &&
                                                                   (ff2 == nullptr || f7f8 != ff2->ffInfo.d)))) {
                    if (mux_output_used) {
                        DBG();
                        return false; // Memory and SRLs only valid in SLICEMs
                    }
                    mux_output_used = true;
                }
            }

            lts.eights[i].valid = true;
        } else if (!lts.eights[i].valid) {
            return false;
        }
    }
    // Check half-tiles
    for (int i = 0; i < 2; i++) {
        if (lts.halfs[i].dirty) {
            lts.halfs[i].valid = false;
            bool found_ff[2] = {false, false};
            NetInfo *clk = nullptr, *sr = nullptr, *ce[2] = {nullptr};
            bool clkinv = false, srinv = false, islatch = false;
            for (int z = 4 * i; z < 4 * (i + 1); z++) {
                for (int k = 0; k < 2; k++) {
                    CellInfo *ff = lts.cells[z << 4 | (BEL_FF + k)];
                    if (ff == nullptr)
                        continue;
                    if (found_ff[0] || found_ff[1]) {
                        if (ff->ffInfo.clk != clk)
                            return false;
                        if (ff->ffInfo.sr != sr)
                            return false;
                        if (ff->ffInfo.is_clkinv != clkinv)
                            return false;
                        if (ff->ffInfo.is_srinv != srinv)
                            return false;
                        if (ff->ffInfo.is_latch != islatch)
                            return false;
                    } else {
                        clk = ff->ffInfo.clk;
                        sr = ff->ffInfo.sr;
                        clkinv = ff->ffInfo.is_clkinv;
                        srinv = ff->ffInfo.is_srinv;
                        islatch = ff->ffInfo.is_latch;
                    }
                    if (found_ff[k]) {
                        if (ff->ffInfo.ce != ce[k])
                            return false;
                    } else {
                        ce[k] = ff->ffInfo.ce;
                    }
                    found_ff[k] = true;
                }
            }
            lts.halfs[i].valid = true;
        } else if (!lts.halfs[i].valid) {
            return false;
        }
    }
    return true;
}

bool Arch::xc7_logic_tile_valid(IdString tileType, LogicTileStatus &lts) const
{
    bool is_slicem = (tileType == id_CLBLM_L) || (tileType == id_CLBLM_R);
    bool tile_is_memory = false;
    if (lts.cells[(3 << 4) | BEL_6LUT] != nullptr && lts.cells[(3 << 4) | BEL_6LUT]->lutInfo.is_memory)
        tile_is_memory = true;
    NetInfo *wclk = nullptr;
    // Check eight-tiles (mostly LUT-related validity)
    for (int i = 0; i < 8; i++) {
        if (lts.eights[i].dirty) {
            lts.eights[i].dirty = false;
            lts.eights[i].valid = false;

            CellInfo *lut6 = lts.cells[(i << 4) | BEL_6LUT];
            CellInfo *lut5 = lts.cells[(i << 4) | BEL_5LUT];

            // Check 6LUT
            if (lut6 != nullptr) {
                if (!is_slicem && (lut6->lutInfo.is_memory || lut6->lutInfo.is_srl))
                    return false; // Memory and SRLs only valid in SLICEMs
                if (lut6->lutInfo.is_memory) {
                    if (wclk == nullptr)
                        wclk = lut6->lutInfo.wclk;
                    else if (lut6->lutInfo.wclk != wclk) {
                        DBG();
                        return false;
                    }
                }
                if (lut5 != nullptr) {
                    // Can't mix memory and non-memory
                    if (lut6->lutInfo.is_memory != lut5->lutInfo.is_memory ||
                        lut6->lutInfo.is_srl != lut5->lutInfo.is_srl)
                        return false;
                    // If all 6 inputs or 2 outputs are used, 5LUT can't also be present
                    if (lut6->lutInfo.input_count == 6 || lut6->lutInfo.output_count == 2)
                        return false;
                    // If more than 5 total inputs are used, need to check number of shared input
                    if ((lut6->lutInfo.input_count + lut5->lutInfo.input_count) > 5) {
                        int shared = 0, need_shared = (lut6->lutInfo.input_count + lut5->lutInfo.input_count - 5);
                        for (int j = 0; j < lut6->lutInfo.input_count; j++) {
                            for (int k = 0; k < lut5->lutInfo.input_count; k++) {
                                if (lut6->lutInfo.input_sigs[j] == lut5->lutInfo.input_sigs[k])
                                    shared++;
                                if (shared >= need_shared)
                                    break;
                            }
                        }
                        if (shared < need_shared) {
                            DBG();
                            return false;
                        }
                    }
                }
            }
            if (lut5 != nullptr) {
                if (!is_slicem && (lut5->lutInfo.is_memory || lut5->lutInfo.is_srl)) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
                // 5LUT can use at most 5 inputs and 1 output
                if (lut5->lutInfo.input_count > 5 || lut5->lutInfo.output_count == 2) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
            }

            // Check (over)usage ofX inputs
            NetInfo *x_net = nullptr;
            if (lut6 != nullptr) {
                x_net = lut6->lutInfo.di2_net;
            }

            CellInfo *mux = nullptr;
            // Eights A, C, E, G: F7MUX uses X input
            if (i == 0 || i == 2 || i == 4 || i == 6)
                mux = lts.cells[i << 4 | BEL_F7MUX];
            // Eights B, F: F8MUX uses X input
            if (i == 1 || i == 5)
                mux = lts.cells[(i - 1) << 4 | BEL_F8MUX];

            if (mux != nullptr) {
                if (x_net == nullptr)
                    x_net = mux->muxInfo.sel;
                else if (x_net != mux->muxInfo.sel) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
            }

            CellInfo *out_fmux = nullptr;
            // Eights B, D, F, H: F7MUX connects to F7F8 out
            if (i == 1 || i == 3 || i == 5 || i == 7)
                out_fmux = lts.cells[(i - 1) << 4 | BEL_F7MUX];
            // Eights C, G: F8MUX connects to F7F8 out
            if (i == 2 || i == 6)
                out_fmux = lts.cells[(i - 2) << 4 | BEL_F8MUX];

            CellInfo *carry4 = lts.cells[((i / 4) << 6) | BEL_CARRY4];

            if (carry4 != nullptr && carry4->carryInfo.x_sigs[i % 4] != nullptr) {
                if (x_net == nullptr)
                    x_net = carry4->carryInfo.x_sigs[i % 4];
                else if (x_net != carry4->carryInfo.x_sigs[i % 4]) {
                    DBG();
                    return false;
                }
            }

            // FF1 might use X, if it isn't driven directly
            CellInfo *ff1 = lts.cells[i << 4 | BEL_FF];
            if (ff1 != nullptr && ff1->ffInfo.d != nullptr && ff1->ffInfo.d->driver.cell != nullptr) {
                auto &drv = ff1->ffInfo.d->driver;
                if ((drv.cell == lut6 && drv.port != id_MC31) || drv.cell == lut5 || drv.cell == out_fmux) {
                    // Direct, OK
                } else {
                    // Indirect, must use X input
                    if (x_net == nullptr)
                        x_net = ff1->ffInfo.d;
                    else if (x_net != ff1->ffInfo.d) {
                        DBG();
                        return false;
                    }
                }
            }

            // FF2 might use X, if it isn't driven directly
            CellInfo *ff2 = lts.cells[i << 4 | BEL_FF2];
            if (ff2 != nullptr && ff2->ffInfo.d != nullptr && ff2->ffInfo.d->driver.cell != nullptr) {
                auto &drv = ff2->ffInfo.d->driver;
                if (drv.cell == lut5) {
                    // Direct, OK
                } else {
                    // Indirect, must use X input
                    if (x_net == nullptr)
                        x_net = ff2->ffInfo.d;
                    else if (x_net != ff2->ffInfo.d) {
#ifdef DEBUG_VALIDITY
                        log_info("%s %s %s %s %s\n", nameOf(lut6), nameOf(ff1), nameOf(lut5), nameOf(ff2),
                                 nameOf(drv.cell));
#endif
                        DBG();
                        return false;
                    }
                }
            }

            // collision with top address bits
            if (tile_is_memory) {
                CellInfo *top_lut = lts.cells[(3 << 4) | BEL_6LUT];
                if (top_lut != nullptr) {
                    if ((i == 2) && x_net != top_lut->lutInfo.address_msb[0])
                        return false;
                    if ((i == 1) && x_net != top_lut->lutInfo.address_msb[1])
                        return false;
                }
            }

            bool mux_output_used = false;
            NetInfo *out5 = nullptr;
            if (lut6 != nullptr && lut6->lutInfo.output_count == 2)
                out5 = lut6->lutInfo.output_sigs[1];
            else if (lut5 != nullptr && !lut5->lutInfo.only_drives_carry)
                out5 = lut5->lutInfo.output_sigs[0];
            if (out5 != nullptr && (out5->users.size() > 1 || ((ff1 == nullptr || out5 != ff1->ffInfo.d) &&
                                                               (ff2 == nullptr || out5 != ff2->ffInfo.d)))) {
                mux_output_used = true;
            }

            if (carry4 != nullptr && carry4->carryInfo.out_sigs[i % 4] != nullptr) {
                // FIXME: direct connections to FF
                if (mux_output_used) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
                mux_output_used = true;
            }
            if (out_fmux != nullptr) {
                NetInfo *f7f8 = out_fmux->muxInfo.out;
                if (f7f8 != nullptr && (f7f8->users.size() > 1 || ((ff1 == nullptr || f7f8 != ff1->ffInfo.d)))) {
                    if (mux_output_used) {
                        DBG();
                        return false; // Memory and SRLs only valid in SLICEMs
                    }
                    mux_output_used = true;
                }
            }
            if (ff2 != nullptr) {
                if (mux_output_used) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
                mux_output_used = true;
            }

            lts.eights[i].valid = true;
        } else if (!lts.eights[i].valid) {
            return false;
        }
    }
    // Check half-tiles
    for (int i = 0; i < 2; i++) {
        if (lts.halfs[i].dirty) {
            lts.halfs[i].valid = false;
            bool found_ff[2] = {false, false};
            NetInfo *clk = nullptr, *sr = nullptr, *ce = nullptr;
            bool clkinv = false, srinv = false, islatch = false;
            for (int z = 4 * i; z < 4 * (i + 1); z++) {
                for (int k = 0; k < 2; k++) {
                    CellInfo *ff = lts.cells[z << 4 | (BEL_FF + k)];
                    if (ff == nullptr)
                        continue;
                    if (ff->ffInfo.is_latch && k == 1)
                        return false;
                    if (found_ff[0] || found_ff[1]) {
                        if (ff->ffInfo.clk != clk)
                            return false;
                        if (ff->ffInfo.sr != sr)
                            return false;
                        if (ff->ffInfo.ce != ce)
                            return false;
                        if (ff->ffInfo.is_clkinv != clkinv)
                            return false;
                        if (ff->ffInfo.is_srinv != srinv)
                            return false;
                        if (ff->ffInfo.is_latch != islatch)
                            return false;
                    } else {
                        clk = ff->ffInfo.clk;
                        if (i == 0 && wclk != nullptr && clk != wclk)
                            return false;
                        sr = ff->ffInfo.sr;
                        ce = ff->ffInfo.ce;
                        clkinv = ff->ffInfo.is_clkinv;
                        srinv = ff->ffInfo.is_srinv;
                        islatch = ff->ffInfo.is_latch;
                    }
                    found_ff[k] = true;
                }
            }
            lts.halfs[i].valid = true;
        } else if (!lts.halfs[i].valid) {
            return false;
        }
    }
    return true;
}

bool Arch::isBelLocationValid(BelId bel) const
{
    IdString belTileType = getBelTileType(bel);
    if (isLogicTile(bel)) {
        // Logic Tile
        if (!tileStatus[bel.tile].lts)
            return true;
        LogicTileStatus &lts = *(tileStatus[bel.tile].lts);
        if (xc7)
            return xc7_logic_tile_valid(belTileType, lts);
        else
            return xcu_logic_tile_valid(belTileType, lts);
    } else if (belTileType == id_BRAM || belTileType == id_BRAM_L || belTileType == id_BRAM_R) {
        if (!tileStatus[bel.tile].bts)
            return true;
        BRAMTileStatus *bts = tileStatus[bel.tile].bts;
        auto onehot = [&](CellInfo *a, CellInfo *b, CellInfo *c) {
            return (((a != nullptr) ? 1 : 0) + ((b != nullptr) ? 1 : 0) + ((c != nullptr) ? 1 : 0)) <= 1;
        };
        // Only one type of BRAM cell at any given location
        if (!onehot(bts->cells[BEL_RAMFIFO36], bts->cells[BEL_RAM36], bts->cells[BEL_FIFO36])) {
            DBG();
            return false;
        }
        if (!onehot(bts->cells[BEL_RAMFIFO18_L], bts->cells[BEL_RAM18_L], bts->cells[BEL_FIFO18_L])) {
            DBG();
            return false;
        }
        // 18-bit BRAMs cannot be used whilst 36-bit is used
        if (bts->cells[BEL_RAMFIFO36] || bts->cells[BEL_RAM36] || bts->cells[BEL_FIFO36]) {
            for (int i = 4; i < 12; i++)
                if (bts->cells[i]) {
                    DBG();
                    return false;
                }
        }
    } else {
        for (auto bel : getBelsByTile(bel.tile % chip_info->width, bel.tile / chip_info->width))
            if (getBoundBelCell(bel) != nullptr && usp_bel_hard_unavail(bel))
                return false;
    }
    return true;
}

bool Arch::isValidBelForCell(CellInfo *cell, BelId bel) const
{
    if (usp_bel_hard_unavail(bel))
        return false;
    return true;
}

void Arch::fixupPlacement()
{
    log_info("Running post-placement legalisation...\n");
    for (auto &ts : tileStatus) {
        if (ts.lts == nullptr)
            continue;
        auto &lt = *(ts.lts);
        for (int z = 0; z < 8; z++) {
            // Fixup LUT connectivity - applies whenever a LUT5 is used
            CellInfo *lut5 = lt.cells[z << 4 | BEL_5LUT];
            if (lut5 == nullptr)
                continue;
            std::unordered_map<IdString, std::vector<int>> lut5Inputs, lut6Inputs;
            for (int i = 0; i < lut5->lutInfo.input_count; i++)
                if (lut5->lutInfo.input_sigs[i])
                    lut5Inputs[lut5->lutInfo.input_sigs[i]->name].push_back(i);
            CellInfo *lut6 = lt.cells[z << 4 | BEL_6LUT];
            if (lut6) {
                for (int i = 0; i < lut6->lutInfo.input_count; i++)
                    if (lut6->lutInfo.input_sigs[i])
                        lut6Inputs[lut6->lutInfo.input_sigs[i]->name].push_back(i);
            }
            std::set<IdString> uniqueInputs;
            for (auto i5 : lut5Inputs)
                uniqueInputs.insert(i5.first);
            for (auto i6 : lut6Inputs)
                uniqueInputs.insert(i6.first);
            // Disconnect LUT inputs, and re-connect them to not overlap
            IdString ports[6] = {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6};
            for (auto p : ports) {
                disconnect_port(getCtx(), lut5, p);
                lut5->attrs.erase(id("X_ORIG_PORT_" + p.str(this)));
                if (lut6) {
                    lut6->attrs.erase(id("X_ORIG_PORT_" + p.str(this)));
                    disconnect_port(getCtx(), lut6, p);
                }
            }
            int index = 0;
            for (auto i : uniqueInputs) {
                if (lut5Inputs.count(i)) {
                    if (!lut5->ports.count(ports[index])) {
                        lut5->ports[ports[index]].name = ports[index];
                        lut5->ports[ports[index]].type = PORT_IN;
                    }
                    connect_port(getCtx(), nets.at(i).get(), lut5, ports[index]);
                    lut5->attrs[id("X_ORIG_PORT_" + ports[index].str(this))] = std::string("");
                    bool first = true;
                    for (auto inp : lut5Inputs[i]) {
                        lut5->attrs[id("X_ORIG_PORT_" + ports[index].str(this))].str +=
                                (first ? "I" : " I") + std::to_string(inp);
                        first = false;
                    }
                }
                if (lut6 && lut6Inputs.count(i)) {
                    if (!lut6->ports.count(ports[index])) {
                        lut6->ports[ports[index]].name = ports[index];
                        lut6->ports[ports[index]].type = PORT_IN;
                    }
                    connect_port(getCtx(), nets.at(i).get(), lut6, ports[index]);

                    lut6->attrs[id("X_ORIG_PORT_" + ports[index].str(this))] = std::string("");
                    bool first = true;
                    for (auto inp : lut6Inputs[i]) {
                        lut6->attrs[id("X_ORIG_PORT_" + ports[index].str(this))].str +=
                                (first ? "I" : " I") + std::to_string(inp);
                        first = false;
                    }
                }
                ++index;
            }
            rename_port(getCtx(), lut5, id_O6, id_O5);
            lut5->attrs.erase(id("X_ORIG_PORT_O6"));
            lut5->attrs[id("X_ORIG_PORT_O5")] = std::string("O");

            if (lut6) {
                if (!lut6->ports.count(id_A6)) {
                    lut6->ports[id_A6].name = id_A6;
                    lut6->ports[id_A6].type = PORT_IN;
                }
                connect_port(getCtx(), nets[id("$PACKER_VCC_NET")].get(), lut6, id_A6);
            }
        }
    }
    for (auto cell : sorted(cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id("PSS_ALTO_CORE")) {
            log_info("Tieing unused PSS inputs to constants...\n");
            for (IdString pname : getBelPins(ci->bel)) {
                if (ci->ports.count(pname) && ci->ports.at(pname).net != nullptr &&
                    ci->ports.at(pname).net->driver.cell != nullptr)
                    continue;
                if (getBelPinType(ci->bel, pname) == PORT_OUT)
                    continue;
                std::string name = pname.str(this);
                if (name.find("_PAD_") != std::string::npos)
                    continue;
                if (name.find("PSVERSION") != std::string::npos || name.find("PSSGTS") != std::string::npos ||
                    name == "PSSGPWRDWNB" || name == "PSSGHIGHB" || name == "PSSFSTCFGB" || name == "PSSCFGRESETB" ||
                    name == "PCFGPORB" || name.find("IDCODE") != std::string::npos ||
                    name.find("BSCAN") != std::string::npos)
                    continue;
                bool constval = false;
                if (name == "NIRQ1LPDRPU" || name == "NIRQ0LPDRPU" || name == "NFIQ1LPDRPU" || name == "NFIQ0LPDRPU")
                    constval = true;
                if (boost::ends_with(name, "SSIN") || name == "EMIOSDIO1WP" || name == "EMIOSDIO0WP" ||
                    boost::ends_with(name, "RSOP") || boost::ends_with(name, "REOP") ||
                    boost::ends_with(name, "GMIITXCLK") || name == "DPVIDEOINCLK" || name == "DPSAXISAUDIOCLK")
                    constval = true;
                ci->ports[pname].name = pname;
                ci->ports[pname].type = PORT_IN;
                if (ci->ports[pname].net != nullptr) {
                    disconnect_port(getCtx(), ci, pname);
                    ci->attrs.erase(id("X_ORIG_PORT_" + name));
                }
                connect_port(getCtx(), nets[constval ? id("$PACKER_VCC_NET") : id("$PACKER_GND_NET")].get(), ci, pname);
            }
        } else if (ci->type == id("BITSLICE_CONTROL_BEL")) {
            std::unordered_map<IdString, bool> constpins;
            constpins[id("EN_VTC")] = true;
            constpins[id("DLY_TEST_IN")] = false;
            constpins[id("RIU_NIBBLE_SEL")] = false;
            constpins[id("TBYTE_IN0")] = false;
            constpins[id("TBYTE_IN1")] = false;
            constpins[id("TBYTE_IN2")] = false;
            constpins[id("TBYTE_IN3")] = false;

            for (auto p : constpins) {
                if (get_net_or_empty(ci, p.first) != nullptr)
                    continue;
                ci->ports[p.first].name = p.first;
                ci->ports[p.first].type = PORT_IN;
                connect_port(getCtx(), nets[p.second ? id("$PACKER_VCC_NET") : id("$PACKER_GND_NET")].get(), ci,
                             p.first);
            }
        }
    }
    // Update the set of reserved wires
    reserved_wires.clear();

    auto get_bouncewire = [&](int tile, IdString swname) {
        auto &td = chip_info->tile_types[chip_info->tile_insts[tile].type];
        WireId sitewire;
        for (int i = 0; i < td.num_wires; i++) {
            auto &w = td.wire_data[i];
            if (w.site != -1 && w.name == swname.index) {
                sitewire.tile = tile;
                sitewire.index = i;
                break;
            }
        }
        NPNR_ASSERT(sitewire != WireId());
        WireId cursor = sitewire;
        while (wireIntent(cursor) != ID_NODE_PINBOUNCE) {
            auto uh = getPipsUphill(cursor);
            NPNR_ASSERT(uh.begin() != uh.end());
            cursor = getPipSrcWire(*(uh.begin()));
        }
        return cursor;
    };
    if (!xc7) {
        for (int tile = 0; tile < chip_info->num_tiles; ++tile) {
            auto &ts = tileStatus[tile];
            if (ts.lts == nullptr)
                continue;
            auto &lts = *(ts.lts);
            for (int z = 0; z < 8; z++) {
                CellInfo *lut5 = lts.cells[z << 4 | BEL_5LUT];
                CellInfo *lut6 = lts.cells[z << 4 | BEL_6LUT];
                NetInfo *i_net = nullptr, *x_net = nullptr;
                // Check usage of DI and X inputs
                if (lut6 != nullptr) {
                    i_net = lut6->lutInfo.di1_net;
                    x_net = lut6->lutInfo.di2_net;
                }
                if (lut5 != nullptr) {
                    if (lut5->lutInfo.di1_net != nullptr)
                        i_net = lut5->lutInfo.di1_net;
                }

                CellInfo *mux = nullptr;
                // Eights A, C, E, G: F7MUX uses X input
                if (z == 0 || z == 2 || z == 4 || z == 6)
                    mux = lts.cells[z << 4 | BEL_F7MUX];
                // Eights B, F: F8MUX uses X input
                if (z == 1 || z == 5)
                    mux = lts.cells[(z - 1) << 4 | BEL_F8MUX];
                // Eights D: F9MUX uses X input
                if (z == 3)
                    mux = lts.cells[BEL_F9MUX];

                if (mux != nullptr)
                    if (x_net == nullptr)
                        x_net = mux->muxInfo.sel;

                CellInfo *carry8 = lts.cells[BEL_CARRY8];
                // CARRY8 might use X
                if (carry8 != nullptr && carry8->carryInfo.x_sigs[z] != nullptr) {
                    if (x_net == nullptr)
                        x_net = carry8->carryInfo.x_sigs[z];
                }

                CellInfo *out_fmux = nullptr;
                // Eights B, D, F, H: F7MUX connects to F7F8 out
                if (z == 1 || z == 3 || z == 5 || z == 7)
                    out_fmux = lts.cells[(z - 1) << 4 | BEL_F7MUX];
                // Eights C, G: F8MUX connects to F7F8 out
                if (z == 2 || z == 6)
                    out_fmux = lts.cells[(z - 2) << 4 | BEL_F8MUX];
                // Eights E: F9MUX connects to F7F8 out
                if (z == 4)
                    out_fmux = lts.cells[BEL_F9MUX];

                // FF1 might use X, if it isn't driven directly
                CellInfo *ff1 = lts.cells[z << 4 | BEL_FF];
                if (ff1 != nullptr && ff1->ffInfo.d != nullptr && ff1->ffInfo.d->driver.cell != nullptr) {
                    auto &drv = ff1->ffInfo.d->driver;
                    if ((drv.cell == lut6 && drv.port != id_MC31) || drv.cell == lut5 || drv.cell == out_fmux) {
                        // Direct, OK
                    } else {
                        // Indirect, must use X input
                        x_net = ff1->ffInfo.d;
                    }
                }

                // FF2 might use I, if it isn't driven directly
                CellInfo *ff2 = lts.cells[z << 4 | BEL_FF2];
                if (ff2 != nullptr && ff2->ffInfo.d != nullptr && ff2->ffInfo.d->driver.cell != nullptr) {
                    auto &drv = ff2->ffInfo.d->driver;
                    if ((drv.cell == lut6 && drv.port != id_MC31) || drv.cell == lut5 || drv.cell == out_fmux) {
                        // Direct, OK
                    } else {
                        // Indirect, must use X input
                        i_net = ff2->ffInfo.d;
                    }
                }

                if (x_net != nullptr) {
                    WireId x_wire = get_bouncewire(tile, id(std::string("") + char('A' + z) + std::string("X")));
                    reserved_wires[x_wire] = x_net;
                }
                if (i_net != nullptr) {
                    WireId i_wire = get_bouncewire(tile, id(std::string("") + char('A' + z) + std::string("_I")));
                    reserved_wires[i_wire] = i_net;
                }
            }
        }
    }
}

void Arch::fixupRouting()
{
    log_info("Running post-routing legalisation...\n");
    /*
     * Convert LUT permutation into correct physical connections (i.e. effectively eliminating the permutation pips),
     * then specifying the permutation as a new physical-to-logical mapping using X_ORIG_PORT. This keeps RapidWright
     * and Vivado happy, preserving the original logical netlist
     */
    std::unordered_map<int, std::vector<int>> used_perm_pips; // tile -> [extra_data] for LUT perm pips

    for (auto net : sorted(nets)) {
        NetInfo *ni = net.second;
        for (auto &wire : ni->wires) {
            PipId pip = wire.second.pip;
            if (pip == PipId())
                continue;
            auto &pd = locInfo(pip).pip_data[pip.index];
            if (pd.flags != PIP_LUT_PERMUTATION)
                continue;
            used_perm_pips[pip.tile].push_back(pd.extra_data);
        }
    }

    for (size_t ti = 0; ti < tileStatus.size(); ti++) {
        if (!used_perm_pips.count(int(ti)))
            continue;
        auto &ts = tileStatus.at(ti);
        if (ts.lts == nullptr)
            continue;

        auto &lt = *(ts.lts);
        for (int z = 0; z < 8; z++) {
            CellInfo *lut5 = lt.cells[z << 4 | BEL_5LUT];
            CellInfo *lut6 = lt.cells[z << 4 | BEL_6LUT];
            if (lut5 == nullptr && lut6 == nullptr)
                continue;
            auto &pp = used_perm_pips.at(ti);
            // from -> to
            std::unordered_map<IdString, std::vector<IdString>> new_connections;
            IdString ports[6] = {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6};
            for (auto pip : pp) {
                if (((pip >> 8) & 0xF) != z)
                    continue;
                new_connections[ports[(pip >> 4) & 0xF]].push_back(ports[pip & 0xF]);
            }
            std::unordered_map<IdString, NetInfo *> orig_nets;
            std::unordered_map<IdString, std::string> orig_ports_l6, orig_ports_l5;
            for (int i = 0; i < 6; i++) {
                NetInfo *l6net = lut6 ? get_net_or_empty(lut6, ports[i]) : nullptr;
                NetInfo *l5net = lut5 ? get_net_or_empty(lut5, ports[i]) : nullptr;
                orig_nets[ports[i]] = (l6net ? l6net : l5net);
                if (lut6)
                    orig_ports_l6[ports[i]] = str_or_default(lut6->attrs, id("X_ORIG_PORT_" + ports[i].str(this)));
                if (lut5)
                    orig_ports_l5[ports[i]] = str_or_default(lut5->attrs, id("X_ORIG_PORT_" + ports[i].str(this)));
            }
            for (auto &nc : new_connections) {
                if (lut6)
                    disconnect_port(getCtx(), lut6, nc.first);
                if (lut5)
                    disconnect_port(getCtx(), lut5, nc.first);
                for (auto &dst : nc.second) {
                    if (lut6)
                        disconnect_port(getCtx(), lut6, dst);
                    if (lut5)
                        disconnect_port(getCtx(), lut5, dst);
                }
            }
            for (int i = 0; i < 6; i++) {
                if (lut6)
                    lut6->attrs.erase(id("X_ORIG_PORT_" + ports[i].str(this)));
                if (lut5)
                    lut5->attrs.erase(id("X_ORIG_PORT_" + ports[i].str(this)));
            }
            for (int i = 0; i < 6; i++) {
                auto p = ports[i];
                if (!new_connections.count(p) || new_connections.at(p).empty())
                    continue;
                if (lut6) {
                    if (!lut6->ports.count(p)) {
                        lut6->ports[p].name = p;
                        lut6->ports[p].type = PORT_IN;
                    }
                    connect_port(getCtx(), orig_nets[new_connections.at(p).front()], lut6, p);
                    lut6->attrs[id("X_ORIG_PORT_" + p.str(this))] = std::string("");
                    auto &orig_attr = lut6->attrs[id("X_ORIG_PORT_" + p.str(this))].str;
                    bool first = true;
                    for (auto &nc : new_connections.at(p)) {
                        orig_attr += orig_ports_l6[nc] + (first ? "" : " ");
                        first = false;
                    }
                    if (orig_attr.empty())
                        lut6->attrs.erase(id("X_ORIG_PORT_" + p.str(this)));
                }
                if (lut5) {
                    if (!lut5->ports.count(p)) {
                        lut5->ports[p].name = p;
                        lut5->ports[p].type = PORT_IN;
                    }
                    connect_port(getCtx(), orig_nets[new_connections.at(p).front()], lut5, p);
                    lut5->attrs[id("X_ORIG_PORT_" + p.str(this))] = std::string("");
                    auto &orig_attr = lut5->attrs[id("X_ORIG_PORT_" + p.str(this))].str;
                    bool first = true;
                    for (auto &nc : new_connections.at(p)) {
                        orig_attr += orig_ports_l5[nc] + (first ? "" : " ");
                        first = false;
                    }
                    if (orig_attr.empty())
                        lut5->attrs.erase(id("X_ORIG_PORT_" + p.str(this)));
                }
            }
        }
    }
    /*
     * Route PAD nets which won't have been routed due to inout issues
     */
    auto route_bfs = [&](NetInfo *net, WireId src, WireId dst) {
        if (src == dst)
            return;
        std::queue<WireId> visit;
        std::unordered_map<WireId, PipId> backtrace;
        visit.push(dst);
        WireId cursor;
        while (!visit.empty()) {
            WireId curr = visit.front();
            visit.pop();
            for (auto uh : getPipsUphill(curr)) {
                if (!checkPipAvail(uh))
                    continue;
                WireId pip_src = getPipSrcWire(uh);
                if (pip_src == src) {
                    cursor = curr;
                    break;
                }
                if (backtrace.count(pip_src))
                    continue;
                if (!checkWireAvail(pip_src))
                    continue;
                backtrace[pip_src] = uh;
                visit.push(pip_src);
            }
        }
        NPNR_ASSERT(cursor != WireId());
        while (backtrace.count(cursor)) {
            auto uh = backtrace[cursor];
            cursor = getPipDstWire(uh);
            if (!getBoundWireNet(cursor))
                bindWire(cursor, net, STRENGTH_STRONG);
            bindPip(uh, net, STRENGTH_STRONG);
        }
    };
    for (auto cell : sorted(cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id("IOB_PAD")) {
            NetInfo *pad_net = ci->ports[id("PAD")].net;
            NPNR_ASSERT(pad_net != nullptr && nets.count(pad_net->name));
            std::vector<WireId> unbind;
            for (auto w : pad_net->wires)
                if (getBoundWireNet(w.first))
                    unbind.push_back(w.first);
            for (auto w : unbind)
                unbindWire(w);
            WireId pad_wire = getBelPinWire(ci->bel, id("PAD"));
            bindWire(pad_wire, pad_net, STRENGTH_LOCKED);
            if (pad_net->driver.cell != nullptr) {
                WireId drv_wire = getCtx()->getNetinfoSourceWire(pad_net);
                if (drv_wire != pad_wire)
                    bindWire(drv_wire, pad_net, STRENGTH_LOCKED);
                route_bfs(pad_net, drv_wire, pad_wire);
            }
            for (auto &usr : pad_net->users) {
                if (usr.cell == ci)
                    continue;
                route_bfs(pad_net, pad_wire, getCtx()->getNetinfoSinkWire(pad_net, usr));
            }
        }
    }

    /*
     * Legalise route through OSERDESE3s
     */
    for (auto cell : sorted(cells)) {
        CellInfo *ci = cell.second;
        if (ci->type == id("OSERDESE3")) {
            if (get_net_or_empty(ci, id("T_OUT")) != nullptr)
                continue;
            ci->params[id("OSERDES_T_BYPASS")] = std::string("TRUE");
        }
    }
}

NEXTPNR_NAMESPACE_END
