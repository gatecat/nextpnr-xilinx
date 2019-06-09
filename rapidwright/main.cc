/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@symbioticeda.com>
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

#ifdef MAIN_EXECUTABLE

#include <fstream>
#include "command.h"
#include "design_utils.h"
#include "jsonwrite.h"
#include "log.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

class UspCommandHandler : public CommandHandler
{
  public:
    UspCommandHandler(int argc, char **argv);
    virtual ~UspCommandHandler(){};
    std::unique_ptr<Context> createContext() override;
    void setupArchContext(Context *ctx) override{};
    void customBitstream(Context *ctx) override;

  protected:
    po::options_description getArchOptions() override;
};

UspCommandHandler::UspCommandHandler(int argc, char **argv) : CommandHandler(argc, argv) {}

po::options_description UspCommandHandler::getArchOptions()
{
    po::options_description specific("Architecture specific options");
    specific.add_options()("chipdb", po::value<std::string>(), "name of chip database binary");
    return specific;
}

void UspCommandHandler::customBitstream(Context *ctx)
{
    if (ctx->debug) {
        std::ofstream out("delays.csv");

        auto bel_inter_xy = [&](BelId bel) {
            auto &chip_info = ctx->chip_info;
            auto &site = chip_info->tile_insts[bel.tile].site_insts[ctx->locInfo(bel).bel_data[bel.index].site];

            if (site.inter_x != -1) {
                return std::make_pair(site.inter_x, site.inter_y);
            } else {
                return std::make_pair(bel.tile % chip_info->width, bel.tile / chip_info->height);
            }
        };

        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            if (ni->driver.cell == nullptr)
                continue;
            if (ctx->getBelGlobalBuf(ni->driver.cell->bel))
                continue;
            auto drv_loc = bel_inter_xy(ni->driver.cell->bel);
            for (auto &usr : ni->users) {
                auto usr_loc = bel_inter_xy(usr.cell->bel);
                out << std::abs(drv_loc.first - usr_loc.first) << ", " << std::abs(drv_loc.second - usr_loc.second)
                    << ", " << ctx->getDelayNS(ctx->getNetinfoRouteDelay(ni, usr)) << std::endl;
            }
        }
    }
}

std::unique_ptr<Context> UspCommandHandler::createContext()
{
    chipArgs.chipdb = vm["chipdb"].as<std::string>();
    return std::unique_ptr<Context>(new Context(chipArgs));
}

int main(int argc, char *argv[])
{
    UspCommandHandler handler(argc, argv);
    return handler.exec();
}

#endif
