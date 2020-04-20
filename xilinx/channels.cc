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

#include "channel_router.h"
#include "nextpnr.h"

#include <queue>

NEXTPNR_NAMESPACE_BEGIN
namespace ChannelRouter {
enum UltrascaleChannelTypes
{
    // Lut N ipins
    CH_IPIN_W_A,
    CH_IPIN_W_B,
    CH_IPIN_W_C,
    CH_IPIN_W_D,
    CH_IPIN_W_E,
    CH_IPIN_W_F,
    CH_IPIN_W_G,
    CH_IPIN_W_H,
    CH_IPIN_E_A,
    CH_IPIN_E_B,
    CH_IPIN_E_C,
    CH_IPIN_E_D,
    CH_IPIN_E_E,
    CH_IPIN_E_F,
    CH_IPIN_E_G,
    CH_IPIN_E_H,
    // Lut N 'bounce'
    CH_BOUNCE_W,
    CH_BOUNCE_E,
    CH_BYPASS_W,
    CH_BYPASS_E,
    // FF N control set
    CH_CTRL_W_ABCD,
    CH_CTRL_W_EFGH,
    CH_CTRL_E_ABCD,
    CH_CTRL_E_EFGH,
    // Slice output pins
    CH_OUTPUT_E,
    CH_OUTPUT_W,
    // 'IMUX' signals
    CH_INT_IMUX_W,
    CH_INT_IMUX_E,

    CH_INT_SDQ_W,
    CH_INT_SDQ_E,

    CH_INODE_W_FT0,
    CH_INODE_W_FT1,
    CH_INODE_E_FT0,
    CH_INODE_E_FT1,

    CH_SDQNODE_W_FT0,
    CH_SDQNODE_W_FT1,
    CH_SDQNODE_E_FT0,
    CH_SDQNODE_E_FT1,

    CH_NN1_W,
    CH_NN1_E,
    CH_NN2_W,
    CH_NN2_E,
    CH_NN4_W,
    CH_NN4_E,
    CH_NN12,

    CH_SS1_W,
    CH_SS1_E,
    CH_SS2_W,
    CH_SS2_E,
    CH_SS4_W,
    CH_SS4_E,
    CH_SS12,

    CH_EE1_W,
    CH_EE1_E,
    CH_EE2_W,
    CH_EE2_E,
    CH_EE4_W,
    CH_EE4_E,
    CH_EE12,

    CH_WW1_W,
    CH_WW1_E,
    CH_WW2_W,
    CH_WW2_E,
    CH_WW4_W,
    CH_WW4_E,
    CH_WW12,

    CHANNEL_COUNT,
};

void setup_channels() {
    std::vector<Channel> channels(CHANNEL_COUNT);
    for (int i = CH_IPIN_W_A; i<= CH_IPIN_E_H; i++) {
        channels[i].type_name = "IPIN_" + std::to_string(i);
        channels[i].dir = DIR_HORIZ;
        channels[i].width = 6;
        channels[i].cost = 5;
    }
    for (int i = CH_BOUNCE_W; i<= CH_BOUNCE_E; i++) {
        bool west = (i <= CH_BOUNCE_W);
        channels[i].type_name = "BOUNCE_" + std::to_string(i);
        channels[i].dir = DIR_VERT;
        channels[i].width = 4;
        channels[i].cost = 5;
    }
    for (int i = CH_BYPASS_W; i<= CH_BYPASS_E; i++) {
        bool west = (i <= CH_BYPASS_W);
        channels[i].type_name = "BYPASS_" + std::to_string(i);
        channels[i].dir = DIR_VERT;
        channels[i].width = 12;
        channels[i].cost = 5;
    }
    for (int i = CH_INT_IMUX_W; i<= CH_INT_IMUX_E; i++) {
        bool west = (i <= CH_INT_IMUX_W);
        channels[i].type_name = "INT_IMUX_" + std::to_string(i);
        channels[i].dir = DIR_VERT;
        channels[i].width = 112/2;
        channels[i].cost = 5;
    }
    for (int i = CH_INT_SDQ_W; i<= CH_INT_SDQ_E; i++) {
        bool west = (i <= CH_INT_SDQ_W);
        channels[i].type_name = "INT_SDQ_" + std::to_string(i);
        channels[i].dir = DIR_VERT;
        channels[i].width = 182/2;
        channels[i].cost = 5;
    }
    for (int i = CH_INODE_W_FT0; i<= CH_INODE_E_FT1; i++) {
        bool west = (i <= CH_INODE_W_FT1);
        channels[i].type_name = "INODE_" + std::to_string(i);
        channels[i].dir = DIR_VERT;
        channels[i].width = 4;
        channels[i].cost = 5;
    }
    for (int i = CH_SDQNODE_W_FT0; i<= CH_SDQNODE_E_FT1; i++) {
        bool west = (i <= CH_SDQNODE_W_FT1);
        channels[i].type_name = "SDQNODE_" + std::to_string(i);
        channels[i].dir = DIR_VERT;
        channels[i].width = (i == CH_SDQNODE_W_FT1 || i == CH_SDQNODE_E_FT1 ) ? 2 : 3;
        channels[i].cost = 5;
    }
}
}

NEXTPNR_NAMESPACE_END
