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
    CHANNEL_COUNT
};
std::vector<Channel> setup_channel_types()
{
    std::vector<Channel> channels(CHANNEL_COUNT);

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

    channels[CH_H01E].type_name = "H01E";
    channels[CH_H01E].dir = DIR_HORIZ;
    channels[CH_H01E].width = 2;
    channels[CH_H01E].cost = 12;
    channels[CH_H01E].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_H01E].downhill.emplace_back(1, 0, CH_IPIN);
    add_general(channels[CH_H01E].downhill, 0);

    channels[CH_H01W].type_name = "H01W";
    channels[CH_H01W].dir = DIR_HORIZ;
    channels[CH_H01W].width = 2;
    channels[CH_H01W].cost = 12;
    channels[CH_H01W].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_H01W].downhill.emplace_back(-1, 0, CH_IPIN);
    add_general(channels[CH_H01W].downhill, 0);

    channels[CH_V01N].type_name = "V01N";
    channels[CH_V01N].dir = DIR_VERT;
    channels[CH_V01N].width = 2;
    channels[CH_V01N].cost = 12;
    channels[CH_V01N].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_V01N].downhill.emplace_back(-1, 0, CH_IPIN);
    add_general(channels[CH_V01N].downhill, 0);

    channels[CH_V01S].type_name = "V01S";
    channels[CH_V01S].dir = DIR_VERT;
    channels[CH_V01S].width = 2;
    channels[CH_V01S].cost = 12;
    channels[CH_V01S].downhill.emplace_back(0, 0, CH_IPIN);
    channels[CH_V01S].downhill.emplace_back(1, 0, CH_IPIN);
    add_general(channels[CH_V01S].downhill, 0);

    const int x2_decrement = 2;

    channels[CH_H02E].type_name = "H02E";
    channels[CH_H02E].dir = DIR_HORIZ;
    channels[CH_H02E].width = 8 - x2_decrement;
    channels[CH_H02E].cost = 15;
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

    channels[CH_H02W].type_name = "H02W";
    channels[CH_H02W].dir = DIR_HORIZ;
    channels[CH_H02W].width = 8 - x2_decrement;
    channels[CH_H02W].cost = 15;
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

    channels[CH_V02N].type_name = "V02N";
    channels[CH_V02N].dir = DIR_VERT;
    channels[CH_V02N].width = 8 - x2_decrement;
    channels[CH_V02N].cost = 15;
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

    channels[CH_V02S].type_name = "V02N";
    channels[CH_V02S].dir = DIR_VERT;
    channels[CH_V02S].width = 8 - x2_decrement;
    channels[CH_V02S].cost = 15;
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

    const int x6_decrement = 1;

    channels[CH_H06E].type_name = "H06E";
    channels[CH_H06E].dir = DIR_HORIZ;
    channels[CH_H06E].width = 4 - x6_decrement;
    channels[CH_H06E].cost = 20;
    channels[CH_H06E].downhill.emplace_back(-3, 1, CH_H01E);
    channels[CH_H06E].downhill.emplace_back(-3, 0, CH_H01W);
    channels[CH_H06E].downhill.emplace_back(-3, 1, CH_H02E);
    channels[CH_H06E].downhill.emplace_back(-3, 3, CH_H06E);
    channels[CH_H06E].downhill.emplace_back(0, -1, CH_V02N);
    channels[CH_H06E].downhill.emplace_back(-3, -1, CH_V02N);
    channels[CH_H06E].downhill.emplace_back(0, -3, CH_V06N);
    channels[CH_H06E].downhill.emplace_back(-3, -3, CH_V06N);
    channels[CH_H06E].downhill.emplace_back(0, 1, CH_V02S);
    channels[CH_H06E].downhill.emplace_back(-3, 1, CH_V02S);
    channels[CH_H06E].downhill.emplace_back(0, 3, CH_V06S);
    channels[CH_H06E].downhill.emplace_back(-3, 3, CH_V06S);
    channels[CH_H06E].downhill.emplace_back(-3, -1, CH_H02W);

    channels[CH_H06W].type_name = "H06W";
    channels[CH_H06W].dir = DIR_HORIZ;
    channels[CH_H06W].width = 4 - x6_decrement;
    channels[CH_H06W].cost = 20;
    channels[CH_H06W].downhill.emplace_back(3, 1, CH_H01E);
    channels[CH_H06W].downhill.emplace_back(3, 0, CH_H01W);
    channels[CH_H06W].downhill.emplace_back(3, -1, CH_H02W);
    channels[CH_H06W].downhill.emplace_back(3, -3, CH_H06W);
    channels[CH_H06W].downhill.emplace_back(0, -1, CH_V02N);
    channels[CH_H06W].downhill.emplace_back(3, -1, CH_V02N);
    channels[CH_H06W].downhill.emplace_back(0, -3, CH_V06N);
    channels[CH_H06W].downhill.emplace_back(3, -3, CH_V06N);
    channels[CH_H06W].downhill.emplace_back(0, 1, CH_V02S);
    channels[CH_H06W].downhill.emplace_back(3, 1, CH_V02S);
    channels[CH_H06W].downhill.emplace_back(0, 3, CH_V06S);
    channels[CH_H06W].downhill.emplace_back(3, 3, CH_V06S);
    channels[CH_H06W].downhill.emplace_back(3, 1, CH_H02E);

    channels[CH_V06N].type_name = "V06N";
    channels[CH_V06N].dir = DIR_VERT;
    channels[CH_V06N].width = 4 - x6_decrement;
    channels[CH_V06N].cost = 20;
    channels[CH_V06N].downhill.emplace_back(3, -1, CH_V01N);
    channels[CH_V06N].downhill.emplace_back(3, 0, CH_V01S);
    channels[CH_V06N].downhill.emplace_back(0, 1, CH_H02E);
    channels[CH_V06N].downhill.emplace_back(3, 1, CH_H02E);
    channels[CH_V06N].downhill.emplace_back(0, 3, CH_H06E);
    channels[CH_V06N].downhill.emplace_back(3, 3, CH_H06E);
    channels[CH_V06N].downhill.emplace_back(3, -1, CH_V02N);
    channels[CH_V06N].downhill.emplace_back(3, -3, CH_V06N);
    channels[CH_V06N].downhill.emplace_back(3, 1, CH_V02S);
    channels[CH_V06N].downhill.emplace_back(0, -1, CH_H02W);
    channels[CH_V06N].downhill.emplace_back(3, -1, CH_H02W);
    channels[CH_V06N].downhill.emplace_back(0, -3, CH_H06W);
    channels[CH_V06N].downhill.emplace_back(3, -3, CH_H06W);

    channels[CH_V06S].type_name = "V06S";
    channels[CH_V06S].dir = DIR_VERT;
    channels[CH_V06S].width = 4 - x6_decrement;
    channels[CH_V06S].cost = 20;
    channels[CH_V06S].downhill.emplace_back(-3, -1, CH_V01N);
    channels[CH_V06S].downhill.emplace_back(-3, 0, CH_V01S);
    channels[CH_V06S].downhill.emplace_back(0, 1, CH_H02E);
    channels[CH_V06S].downhill.emplace_back(-3, 1, CH_H02E);
    channels[CH_V06S].downhill.emplace_back(-0, 3, CH_H06E);
    channels[CH_V06S].downhill.emplace_back(-3, 3, CH_H06E);
    channels[CH_V06S].downhill.emplace_back(-3, -1, CH_V02N);
    channels[CH_V06S].downhill.emplace_back(-3, 3, CH_V06S);
    channels[CH_V06S].downhill.emplace_back(-3, 1, CH_V02S);
    channels[CH_V06S].downhill.emplace_back(0, -1, CH_H02W);
    channels[CH_V06S].downhill.emplace_back(-3, -1, CH_H02W);
    channels[CH_V06S].downhill.emplace_back(0, -3, CH_H06W);
    channels[CH_V06S].downhill.emplace_back(-3, -3, CH_H06W);

    return channels;
}

struct Ecp5ChannelGraph : ChannelGraph
{
    Context *ctx;
    Ecp5ChannelGraph(Context *ctx) : ctx(ctx){};

    int get_width() const override { return ctx->chip_info->width; }
    int get_height() const override { return ctx->chip_info->height; }
    std::vector<Channel> get_channels() const override { return setup_channel_types(); }
    bool is_global_net(const NetInfo *net) const override { return net->is_global; }
    ChannelNode get_source_node(const NetInfo *net) const override
    {
        // Search forward from actual source wire to general interconnect location
        std::queue<WireId> visit;
        visit.push(ctx->getNetinfoSourceWire(net));
        int iter = 0;
        while (!visit.empty() && iter < 200) {
            WireId cursor = visit.front();
            visit.pop();
            for (PipId dh : ctx->getPipsDownhill(cursor)) {
                WireId dst = ctx->getPipDstWire(dh);
                if (is_general_routing(dst)) {
                    Loc l = ctx->getPipLocation(dh);
                    return ChannelNode(l.x, l.y, CH_OPIN);
                }
                visit.push(dst);
            }
            ++iter;
        }
        return ChannelNode();
    }
    ChannelNode get_sink_node(const NetInfo *net, size_t i) const
    {
        auto &usr = net->users.at(i);
        // Skip all dedicated routing
        if (usr.port == id_FCI || usr.port == id_FXA || usr.port == id_FXA || usr.port == id_DI0 || usr.port == id_DI1)
            return ChannelNode();
        // Search backwards from actual sink wire to general interconnect location
        std::queue<WireId> visit;
        visit.push(ctx->getNetinfoSinkWire(net, usr));
        int iter = 0;
        while (!visit.empty() && iter < 200) {
            WireId cursor = visit.front();
            visit.pop();
            for (PipId uh : ctx->getPipsUphill(cursor)) {
                WireId src = ctx->getPipSrcWire(uh);
                if (is_general_routing(src)) {
                    Loc l = ctx->getPipLocation(uh);
                    return ChannelNode(l.x, l.y, CH_IPIN);
                }
                visit.push(src);
            }
            ++iter;
        }
        return ChannelNode();
    }
    ~Ecp5ChannelGraph() {}

  private:
    bool is_general_routing(WireId wire) const
    {
        std::string basename = ctx->getWireBasename(wire).str(ctx);
        if (basename.length() < 3)
            return false;
        std::string prefix = basename.substr(0, 2);
        return prefix == "H0" || prefix == "V0";
    }
};

} // namespace ChannelRouter

void ecp5_channel_router_test(Context *ctx)
{
    ChannelRouter::Ecp5ChannelGraph g(ctx);
    ChannelRouter::run_channelrouter(ctx, &g, ChannelRouter::ChannelRouterCfg());
    NPNR_ASSERT_FALSE("DONE");
}

NEXTPNR_NAMESPACE_END
