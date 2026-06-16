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
    std::unique_ptr<Context> createContext(dict<std::string, Property> &values) override;
    void setupArchContext(Context *ctx) override{};
    void customBitstream(Context *ctx) override;
    void customAfterLoad(Context *ctx) override;

  protected:
    po::options_description getArchOptions() override;
};

UspCommandHandler::UspCommandHandler(int argc, char **argv) : CommandHandler(argc, argv) {}

po::options_description UspCommandHandler::getArchOptions()
{
    po::options_description specific("Architecture specific options");
    specific.add_options()("chipdb", po::value<std::string>(), "name of chip database binary");
    specific.add_options()("xdc", po::value<std::vector<std::string>>(), "XDC-style constraints file");
    specific.add_options()("fasm", po::value<std::string>(), "fasm bitstream file to write");
    specific.add_options()("no-iobs", "disable automatic IO buffer insertion (out-of-context mode)");
    specific.add_options()("out-of-context",
                           "alias for --no-iobs: skip IO buffer insertion and enable nextpnr's "
                           "out-of-context timing analysis (arch.ooc)");

    return specific;
}

void UspCommandHandler::customBitstream(Context *ctx)
{
    if (vm.count("fasm")) {
        std::string filename = vm["fasm"].as<std::string>();
        ctx->writeFasm(filename);
    }
}

std::unique_ptr<Context> UspCommandHandler::createContext(dict<std::string, Property> &values)
{
    ArchArgs chipArgs;
    if (!vm.count("chipdb")) {
        log_error("chip database binary must be provided\n");
    }
    chipArgs.chipdb = vm["chipdb"].as<std::string>();
    auto ctx = std::unique_ptr<Context>(new Context(chipArgs));
    if (vm.count("no-iobs") || vm.count("out-of-context")) {
        // Skip IO buffer insertion so designs whose top-level port count
        // exceeds the device pin count can still place and route.
        ctx->settings[ctx->id("disable_iobs")] = Property::State::S1;
        // Align with nextpnr's generic out-of-context timing analysis:
        // without arch.ooc, common/kernel/timing.cc never seeds top-level
        // inputs as topological start points, so all combinational paths
        // sourced at unbuffered inputs are silently dropped from the
        // timing report.  Matches the pattern used by nextpnr-ecp5.
        ctx->settings[ctx->id("arch.ooc")] = Property::State::S1;
    }
    return ctx;
}

void UspCommandHandler::customAfterLoad(Context *ctx)
{
    if (vm.count("xdc")) {
        std::vector<std::string> files = vm["xdc"].as<std::vector<std::string>>();
        for (const auto &filename : files) {
            std::ifstream in(filename);
            if (!in)
                log_error("failed to open XDC file '%s'\n", filename.c_str());
            ctx->parseXdc(in);
        }
    }
}

int main(int argc, char *argv[])
{
    UspCommandHandler handler(argc, argv);
    return handler.exec();
}

#endif
