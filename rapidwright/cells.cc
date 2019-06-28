#include "cells.h"
#include "design_utils.h"

NEXTPNR_NAMESPACE_BEGIN

std::unique_ptr<CellInfo> create_cell(Context *ctx, IdString type, IdString name)
{
    std::unique_ptr<CellInfo> cell{new CellInfo};
    cell->type = type;
    cell->name = name;
    auto add_port = [&](const std::string &name, PortType dir) {
        IdString id = ctx->id(name);
        cell->ports[id].name = id;
        cell->ports[id].type = dir;
    };
    if (type == id_SLICE_LUTX) {
        for (int i = 1; i <= 6; i++)
            add_port("A" + std::to_string(i), PORT_IN);
        for (int i = 1; i <= 9; i++)
            add_port("WA" + std::to_string(i), PORT_IN);
        add_port("DI1", PORT_IN);
        add_port("DI2", PORT_IN);
        add_port("CLK", PORT_IN);
        add_port("WE", PORT_IN);
        add_port("SIN", PORT_IN);
        add_port("O5", PORT_OUT);
        add_port("O6", PORT_OUT);
        add_port("MC31", PORT_OUT);
    } else if (type == id_SLICE_FFX) {
        add_port("D", PORT_IN);
        add_port("SR", PORT_IN);
        add_port("CE", PORT_IN);
        add_port("CLK", PORT_IN);
        add_port("Q", PORT_OUT);
    } else if (type == ctx->id("RAMD64E")) {
        for (int i = 0; i < 6; i++)
            add_port("RADR" + std::to_string(i), PORT_IN);
        for (int i = 0; i < 8; i++)
            add_port("WADR" + std::to_string(i), PORT_IN);
        add_port("CLK", PORT_IN);
        add_port("I", PORT_IN);
        add_port("WE", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("MUXF7") || type == ctx->id("MUXF8") || type == ctx->id("MUXF9")) {
        add_port("I0", PORT_IN);
        add_port("I1", PORT_IN);
        add_port("S", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("CARRY8")) {
        add_port("CI", PORT_IN);
        add_port("CI_TOP", PORT_IN);

        for (int i = 0; i < 8; i++) {
            add_port("DI[" + std::to_string(i) + "]", PORT_IN);
            add_port("S[" + std::to_string(i) + "]", PORT_IN);
            add_port("CO[" + std::to_string(i) + "]", PORT_OUT);
            add_port("O[" + std::to_string(i) + "]", PORT_OUT);
        }
    } else if (type == ctx->id("MUXCY")) {
        add_port("CI", PORT_IN);
        add_port("DI", PORT_IN);
        add_port("S", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("XORCY")) {
        add_port("CI", PORT_IN);
        add_port("LI", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("PAD")) {
        add_port("PAD", PORT_INOUT);
    } else if (type == ctx->id("INBUF")) {
        add_port("VREF", PORT_IN);
        add_port("PAD", PORT_IN);
        add_port("OSC_EN", PORT_IN);
        for (int i = 0; i < 4; i++)
            add_port("OSC[" + std::to_string(i) + "]", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("IBUFCTRL")) {
        add_port("I", PORT_IN);
        add_port("IBUFDISABLE", PORT_IN);
        add_port("T", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("OBUF") || type == ctx->id("IBUF")) {
        add_port("I", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("OBUFT")) {
        add_port("I", PORT_IN);
        add_port("T", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("OBUFT_DCIEN")) {
        add_port("I", PORT_IN);
        add_port("T", PORT_IN);
        add_port("DCITERMDISABLE", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("DIFFINBUF")) {
        add_port("DIFF_IN_P", PORT_IN);
        add_port("DIFF_IN_N", PORT_IN);
        add_port("OSC_EN[0]", PORT_IN);
        add_port("OSC_EN[1]", PORT_IN);
        for (int i = 0; i < 4; i++)
            add_port("OSC[" + std::to_string(i) + "]", PORT_IN);
        add_port("VREF", PORT_IN);
        add_port("O", PORT_OUT);
        add_port("O_B", PORT_OUT);
    } else if (type == ctx->id("HPIO_VREF")) {
        for (int i = 0; i < 7; i++)
            add_port("FABRIC_VREF_TUNE[" + std::to_string(i) + "]", PORT_IN);
        add_port("VREF", PORT_OUT);
    } else if (type == ctx->id("INV")) {
        add_port("I", PORT_IN);
        add_port("O", PORT_OUT);
    }
    return cell;
}

std::unique_ptr<CellInfo> create_lut(Context *ctx, const std::string &name, const std::vector<NetInfo *> &inputs,
                                     NetInfo *output, const Property &init)
{
    std::unique_ptr<CellInfo> cell{new CellInfo};
    cell->type = ctx->id("LUT" + std::to_string(inputs.size()));
    cell->name = ctx->id(name);
    for (size_t i = 0; i < inputs.size(); i++) {
        IdString ip = ctx->id("I" + std::to_string(i));
        cell->ports[ip].name = ip;
        cell->ports[ip].type = PORT_IN;
        connect_port(ctx, inputs.at(i), cell.get(), ip);
    }
    cell->ports[ctx->id("O")].name = ctx->id("O");
    cell->ports[ctx->id("O")].type = PORT_OUT;
    connect_port(ctx, output, cell.get(), ctx->id("O"));
    cell->params[ctx->id("INIT")] = init;
    return cell;
}

NEXTPNR_NAMESPACE_END