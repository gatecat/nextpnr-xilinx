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
    } else if (type == ctx->id("RAMD32")) {
        for (int i = 0; i < 5; i++)
            add_port("RADR" + std::to_string(i), PORT_IN);
        for (int i = 0; i < 5; i++)
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
    } else if (type == ctx->id("IOBUF")) {
        add_port("I", PORT_IN);
        add_port("T", PORT_IN);
        add_port("O", PORT_OUT);
        add_port("IO", PORT_INOUT);
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
    } else if (type == ctx->id("IDELAYCTRL")) {
        add_port("REFCLK", PORT_IN);
        add_port("RST", PORT_IN);
        add_port("RDY", PORT_OUT);
    } else if (type == ctx->id("IBUF")) {
        add_port("I", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("IBUF_INTERMDISABLE")) {
        add_port("I", PORT_IN);
        add_port("IBUFDISABLE", PORT_IN);
        add_port("INTERMDISABLE", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("IBUFDS")) {
        add_port("I", PORT_IN);
        add_port("IB", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("IBUFDS_INTERMDISABLE_INT")) {
        add_port("I", PORT_IN);
        add_port("IB", PORT_IN);
        add_port("IBUFDISABLE", PORT_IN);
        add_port("INTERMDISABLE", PORT_IN);
        add_port("O", PORT_OUT);
    } else if (type == ctx->id("CARRY4")) {
        add_port("CI", PORT_IN);
        add_port("CYINIT", PORT_IN);
        for (int i = 0; i < 4; i++) {
            add_port("DI[" + std::to_string(i) + "]", PORT_IN);
            add_port("S[" + std::to_string(i) + "]", PORT_IN);
            add_port("CO[" + std::to_string(i) + "]", PORT_OUT);
            add_port("O[" + std::to_string(i) + "]", PORT_OUT);
        }
    }
    return cell;
}

std::unique_ptr<CellInfo> create_dsp_cell(Context *ctx, IdString type, IdString name)
{
    std::unique_ptr<CellInfo> cell{new CellInfo};
    cell->type = type;
    cell->name = name;
    auto add_port = [&](const std::string &name, PortType dir) {
        IdString id = ctx->id(name);
        cell->ports[id].name = id;
        cell->ports[id].type = dir;
    };
    auto add_bus = [&](const std::string &name, PortType dir, int width) {
        for (int i = 0; i < width; i++)
            add_port(name + "[" + std::to_string(i) + "]", dir);
    };
    if (type == ctx->id("DSP_PREADD_DATA")) {
        add_bus("A1_DATA", PORT_IN, 27);
        add_bus("A2_DATA", PORT_IN, 27);
        add_bus("AD", PORT_IN, 27);
        add_bus("B1_DATA", PORT_IN, 18);
        add_bus("B2_DATA", PORT_IN, 18);
        add_port("CEAD", PORT_IN);
        add_port("CED", PORT_IN);
        add_port("CEINMODE", PORT_IN);
        add_port("CLK", PORT_IN);
        add_bus("DIN", PORT_IN, 27);
        add_bus("INMODE", PORT_IN, 5);
        add_port("RSTD", PORT_IN);
        add_port("RSTINMODE", PORT_IN);
        add_bus("A2A1", PORT_OUT, 27);
        add_port("ADDSUB", PORT_OUT);
        add_bus("AD_DATA", PORT_OUT, 27);
        add_bus("B2B1", PORT_OUT, 18);
        add_bus("D_DATA", PORT_OUT, 27);
        add_port("INMODE_2", PORT_OUT);
        add_bus("PREADD_AB", PORT_OUT, 27);
    } else if (type == ctx->id("DSP_PREADD")) {
        add_port("ADDSUB", PORT_IN);
        add_bus("D_DATA", PORT_IN, 27);
        add_port("INMODE2", PORT_IN);
        add_bus("PREADD_AB", PORT_IN, 27);
        add_bus("AD", PORT_OUT, 27);
    } else if (type == ctx->id("DSP_A_B_DATA")) {
        add_bus("ACIN", PORT_IN, 30);
        add_bus("A", PORT_IN, 30);
        add_bus("BCIN", PORT_IN, 18);
        add_bus("B", PORT_IN, 18);
        add_port("CEA1", PORT_IN);
        add_port("CEA2", PORT_IN);
        add_port("CEB1", PORT_IN);
        add_port("CEB2", PORT_IN);
        add_port("CLK", PORT_IN);
        add_port("RSTA", PORT_IN);
        add_port("RSTB", PORT_IN);
        add_bus("A1_DATA", PORT_OUT, 27);
        add_bus("A2_DATA", PORT_OUT, 27);
        add_bus("ACOUT", PORT_OUT, 30);
        add_bus("A_ALU", PORT_OUT, 30);
        add_bus("B1_DATA", PORT_OUT, 18);
        add_bus("B2_DATA", PORT_OUT, 18);
        add_bus("BCOUT", PORT_OUT, 18);
        add_bus("B_ALU", PORT_OUT, 18);
    } else if (type == ctx->id("DSP_MULTIPLIER")) {
        add_bus("A2A1", PORT_IN, 27);
        add_bus("AD_DATA", PORT_IN, 27);
        add_bus("B2B1", PORT_IN, 18);
        add_port("AMULT26", PORT_OUT);
        add_port("BMULT17", PORT_OUT);
        add_bus("U", PORT_OUT, 45);
        add_bus("V", PORT_OUT, 45);
    } else if (type == ctx->id("DSP_C_DATA")) {
        add_port("CEC", PORT_IN);
        add_port("CLK", PORT_IN);
        add_bus("C", PORT_IN, 48);
        add_port("RSTC", PORT_IN);
        add_bus("C_DATA", PORT_OUT, 48);
    } else if (type == ctx->id("DSP_M_DATA")) {
        add_port("CEM", PORT_IN);
        add_port("CLK", PORT_IN);
        add_port("RSTM", PORT_IN);
        add_bus("U", PORT_IN, 45);
        add_bus("V", PORT_IN, 45);
        add_bus("U_DATA", PORT_OUT, 45);
        add_bus("V_DATA", PORT_OUT, 45);
    } else if (type == ctx->id("DSP_ALU")) {
        add_bus("ALUMODE", PORT_IN, 4);
        add_port("AMULT26", PORT_IN);
        add_bus("A_ALU", PORT_IN, 30);
        add_port("BMULT17", PORT_IN);
        add_bus("B_ALU", PORT_IN, 18);
        add_port("CARRYCASCIN", PORT_IN);
        add_port("CARRYIN", PORT_IN);
        add_bus("CARRYINSEL", PORT_IN, 3);
        add_port("CCOUT", PORT_IN);
        add_port("CEALUMODE", PORT_IN);
        add_port("CECARRYIN", PORT_IN);
        add_port("CECTRL", PORT_IN);
        add_port("CEM", PORT_IN);
        add_port("CLK", PORT_IN);
        add_bus("C_DATA", PORT_IN, 48);
        add_port("MULTSIGNIN", PORT_IN);
        add_bus("OPMODE", PORT_IN, 9);
        add_bus("PCIN", PORT_IN, 48);
        add_bus("P_FDBK", PORT_IN, 48);
        add_port("RSTALLCARRYIN", PORT_IN);
        add_port("RSTALUMODE", PORT_IN);
        add_port("RSTCTRL", PORT_IN);
        add_bus("U_DATA", PORT_IN, 45);
        add_bus("V_DATA", PORT_IN, 45);
        add_port("ALUMODE10", PORT_OUT);
        add_bus("ALU_OUT", PORT_OUT, 48);
        add_bus("COUT", PORT_OUT, 4);
        add_port("MULTSIGN_ALU", PORT_OUT);
        add_bus("XOR_MX", PORT_OUT, 8);
    } else if (type == ctx->id("DSP_OUTPUT")) {
        add_port("ALUMODE10", PORT_IN);
        add_bus("ALU_OUT", PORT_IN, 48);
        add_port("CEP", PORT_IN);
        add_port("CLK", PORT_IN);
        add_bus("COUT", PORT_IN, 4);
        add_bus("C_DATA", PORT_IN, 48);
        add_port("MULTSIGN_ALU", PORT_IN);
        add_port("RSTP", PORT_IN);
        add_bus("XOR_MX", PORT_IN, 8);
        add_port("CARRYCASCOUT", PORT_OUT);
        add_bus("CARRYOUT", PORT_OUT, 4);
        add_port("CCOUT_FB", PORT_OUT);
        add_port("MULTSIGNOUT", PORT_OUT);
        add_port("OVERFLOW", PORT_OUT);
        add_port("PATTERN_B_DETECT", PORT_OUT);
        add_port("PATTERN_DETECT", PORT_OUT);
        add_bus("PCOUT", PORT_OUT, 48);
        add_bus("P", PORT_OUT, 48);
        add_bus("P_FDBK", PORT_OUT, 48);
        add_port("P_FDBK_47", PORT_OUT);
        add_port("UNDERFLOW", PORT_OUT);
        add_bus("XOROUT", PORT_OUT, 8);
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
