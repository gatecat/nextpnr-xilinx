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

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace ChannelRouter {
enum ChannelDirection
{
    DIR_EAST,
    DIR_WEST,
    DIR_NORTH,
    DIR_SOUTH,
};
struct ChannelPIP
{
    int src_along, dst_along;
    int dst_type;
};
struct Channel
{
    IdString type_name;
    ChannelDirection dir;
    int length;
    int width;
    float cost;
    std::vector<ChannelPIP> downhill;
};
struct ChannelNode
{
    ChannelNode() : x(-1), y(-1), type(-1){};
    ChannelNode(int x, int y, int type) : x(x), y(y), type(type){};
    int x, y, type;
};
struct ChannelGraph
{
    virtual int get_width() const = 0;
    virtual int get_height() const = 0;
    virtual std::vector<Channel> get_channels() const = 0;
    virtual bool is_global_net(const NetInfo *net) const = 0;
    virtual ChannelNode get_source_node(const NetInfo *net) const = 0;
    virtual ChannelNode get_sink_node(const NetInfo *net, const PortRef &usr) const = 0;
    virtual ~ChannelGraph() = 0;
};

struct ChannelRouterCfg
{
    int bb_margin_x = 3, bb_margin_y = 3;
    int togo_cost_dx = 10, togo_cost_dy = 10, togo_cost_adder = 20;
    float bias_cost_factor = 0.5;
};

}; // namespace ChannelRouter

NEXTPNR_NAMESPACE_END
