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

NEXTPNR_NAMESPACE_BEGIN

namespace ChannelRouter {
enum ChannelTypes
{
    CH_IPIN = 0,
    CH_OPIN,
    CH_BOUNCE,
    CH_H01E,
    CH_H01W,
    CH_V01N,
    CH_V01S,
    CH_H02E,
    CH_H02W,
    CH_V02N,
    CH_V02S,
    CH_H06W,
    CH_H06E,
    CH_V06N,
    CH_V06S,
};
std::vector<Channel> setup_channel_types()
{
    std::vector<Channel> channels(18);

    channels[CH_IPIN].type_name = "IPIN";
    channels[CH_IPIN].dir = DIR_HORIZ;
    channels[CH_IPIN].width = 4 * 8 + 8 + 4 + 4 + 4; // ABCD, M, CE, LSR, CLK
    channels[CH_IPIN].cost = 10;

    channels[CH_OPIN].type_name = "OPIN";
    channels[CH_OPIN].dir = DIR_HORIZ;
    channels[CH_OPIN].width = 2 * 8; // FQ
    channels[CH_OPIN].cost = 10;
    channels[CH_OPIN].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_OPIN].downhill.emplace_back(0, 0, CH_BOUNCE);
    channels[CH_OPIN].downhill.emplace_back(0, 0, CH_V01S);
    channels[CH_OPIN].downhill.emplace_back(0, -1, CH_V01N);
    channels[CH_OPIN].downhill.emplace_back(0, 1, CH_H01E);
    channels[CH_OPIN].downhill.emplace_back(0, -1, CH_H01W);
    channels[CH_OPIN].downhill.emplace_back(0, 1, CH_V02S);
    channels[CH_OPIN].downhill.emplace_back(0, -1, CH_V02N);
    channels[CH_OPIN].downhill.emplace_back(0, -1, CH_H02W);
    channels[CH_OPIN].downhill.emplace_back(0, 1, CH_H02E);

    channels[CH_BOUNCE].type_name = "BOUNCE";
    channels[CH_BOUNCE].dir = DIR_HORIZ;
    channels[CH_BOUNCE].width = 8;
    channels[CH_BOUNCE].cost = 10;
    channels[CH_BOUNCE].downhill.emplace_back(0, 0, CH_IPIN);

    auto add_general = [&](std::vector<ChannelPIP> &downhill, int o) {
        downhill.emplace_back(o, 1, CH_H02E);
        downhill.emplace_back(o, 3, CH_H06E);
        downhill.emplace_back(o, -1, CH_V02N);
        downhill.emplace_back(o, -3, CH_V06N);
        downhill.emplace_back(o, 1, CH_V02S);
        downhill.emplace_back(o, 3, CH_V06S);
        downhill.emplace_back(o, -1, CH_H02W);
        downhill.emplace_back(o, -3, CH_H06W);
    };

    channels[CH_H01E].type_name = "CH_H01E";
    channels[CH_H01E].dir = DIR_HORIZ;
    channels[CH_H01E].width = 2;
    channels[CH_H01E].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_H01E].downhill.emplace_back(1, 0, CH_IPIN);
    add_general(channels[CH_H01E].downhill, 0);

    channels[CH_H01W].type_name = "CH_H01W";
    channels[CH_H01W].dir = DIR_HORIZ;
    channels[CH_H01W].width = 2;
    channels[CH_H01W].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_H01W].downhill.emplace_back(-1, 0, CH_IPIN);
    add_general(channels[CH_H01W].downhill, 0);

    channels[CH_V01N].type_name = "CH_V01N";
    channels[CH_V01N].dir = DIR_VERT;
    channels[CH_V01N].width = 2;
    channels[CH_V01N].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_V01N].downhill.emplace_back(-1, 0, CH_IPIN);
    add_general(channels[CH_V01N].downhill, 0);

    channels[CH_V01S].type_name = "CH_V01S";
    channels[CH_V01S].dir = DIR_VERT;
    channels[CH_V01S].width = 2;
    channels[CH_V01S].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_V01S].downhill.emplace_back(1, 0, CH_IPIN);
    add_general(channels[CH_V01S].downhill, 0);

    channels[CH_H02E].type_name = "CH_H02E";
    channels[CH_H02E].dir = DIR_HORIZ;
    channels[CH_H02E].width = 8;
    channels[CH_H02E].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_H02E].downhill.emplace_back(-1, 0, CH_IPIN);
    channels[CH_H02E].downhill.emplace_back(0, 0, CH_BOUNCE);
    channels[CH_H02E].downhill.emplace_back(-1, 0, CH_BOUNCE);
    channels[CH_H02E].downhill.emplace_back(-1, 1, CH_H02E);
    channels[CH_H02E].downhill.emplace_back(-1, 3, CH_H06E);
    channels[CH_H02E].downhill.emplace_back(0, -1, CH_V02N);
    channels[CH_H02E].downhill.emplace_back(-1, -1, CH_V02N);
    channels[CH_H02E].downhill.emplace_back(0, 1, CH_V02S);
    channels[CH_H02E].downhill.emplace_back(-1, 1, CH_V02S);

    channels[CH_H02W].type_name = "CH_H02W";
    channels[CH_H02W].dir = DIR_HORIZ;
    channels[CH_H02W].width = 8;
    channels[CH_H02W].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_H02W].downhill.emplace_back(1, 0, CH_IPIN);
    channels[CH_H02W].downhill.emplace_back(0, 0, CH_BOUNCE);
    channels[CH_H02W].downhill.emplace_back(1, 0, CH_BOUNCE);
    channels[CH_H02W].downhill.emplace_back(0, -1, CH_V02N);
    channels[CH_H02W].downhill.emplace_back(1, -1, CH_V02N);
    channels[CH_H02W].downhill.emplace_back(0, 1, CH_V02S);
    channels[CH_H02W].downhill.emplace_back(1, 1, CH_V02S);
    channels[CH_H02W].downhill.emplace_back(1, -1, CH_H02W);
    channels[CH_H02W].downhill.emplace_back(1, -3, CH_H06W);

    channels[CH_V02N].type_name = "CH_V02N";
    channels[CH_V02N].dir = DIR_VERT;
    channels[CH_V02N].width = 8;
    channels[CH_V02N].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_V02N].downhill.emplace_back(1, 0, CH_IPIN);
    channels[CH_V02N].downhill.emplace_back(0, 0, CH_BOUNCE);
    channels[CH_V02N].downhill.emplace_back(1, 0, CH_BOUNCE);
    channels[CH_V02N].downhill.emplace_back(0, 1, CH_H02E);
    channels[CH_V02N].downhill.emplace_back(1, 1, CH_H02E);
    channels[CH_V02N].downhill.emplace_back(0, -1, CH_H02W);
    channels[CH_V02N].downhill.emplace_back(1, -1, CH_H02W);
    channels[CH_V02N].downhill.emplace_back(1, -1, CH_V02N);
    channels[CH_V02N].downhill.emplace_back(1, -3, CH_V06N);

    channels[CH_V02S].type_name = "CH_V02N";
    channels[CH_V02S].dir = DIR_VERT;
    channels[CH_V02S].width = 8;
    channels[CH_V02S].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_V02S].downhill.emplace_back(-1, 0, CH_IPIN);
    channels[CH_V02S].downhill.emplace_back(0, 0, CH_BOUNCE);
    channels[CH_V02S].downhill.emplace_back(-1, 0, CH_BOUNCE);
    channels[CH_V02S].downhill.emplace_back(0, 1, CH_H02E);
    channels[CH_V02S].downhill.emplace_back(-1, 1, CH_H02E);
    channels[CH_V02S].downhill.emplace_back(0, -1, CH_H02W);
    channels[CH_V02S].downhill.emplace_back(-1, -1, CH_H02W);
    channels[CH_V02S].downhill.emplace_back(-1, 1, CH_V02S);
    channels[CH_V02S].downhill.emplace_back(-1, 3, CH_V06S);
    return channels;
}
} // namespace ChannelRouter

NEXTPNR_NAMESPACE_END
