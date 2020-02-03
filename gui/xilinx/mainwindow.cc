/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <miodrag@symbioticeda.com>
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

#include "mainwindow.h"
#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <fstream>
#include "design_utils.h"
#include "log.h"

static void initMainResource() { Q_INIT_RESOURCE(nextpnr); }

NEXTPNR_NAMESPACE_BEGIN

MainWindow::MainWindow(std::unique_ptr<Context> context, CommandHandler *handler, QWidget *parent)
        : BaseMainWindow(std::move(context), handler, parent)
{
    initMainResource();

    std::string title = "nextpnr-xilinx - [EMPTY]";
    setWindowTitle(title.c_str());

    connect(this, &BaseMainWindow::contextChanged, this, &MainWindow::newContext);

    createMenu();
}

MainWindow::~MainWindow() {}

void MainWindow::createMenu()
{
    // Add arch specific actions
    actionLoadXDC = new QAction("Open XDC", this);
    actionLoadXDC->setIcon(QIcon(":/icons/resources/open_xdc.png"));
    actionLoadXDC->setStatusTip("Open XDC file");
    actionLoadXDC->setEnabled(false);
    connect(actionLoadXDC, &QAction::triggered, this, &MainWindow::open_xdc);

    actionSaveFASM = new QAction("Save FASM", this);
    actionSaveFASM->setIcon(QIcon(":/icons/resources/save_fasm.png"));
    actionSaveFASM->setStatusTip("Save FASM file");
    actionSaveFASM->setEnabled(false);
    connect(actionSaveFASM, &QAction::triggered, this, &MainWindow::save_fasm);

    // Add actions in menus
    mainActionBar->addSeparator();
    mainActionBar->addAction(actionLoadXDC);
    mainActionBar->addAction(actionSaveFASM);

    menuDesign->addSeparator();
    menuDesign->addAction(actionLoadXDC);
    menuDesign->addAction(actionSaveFASM);
}

void MainWindow::new_proj()
{
    QString fileName = QFileDialog::getOpenFileName(this, QString("Open chip database"), QString(), QString("*.bin"));
    if (!fileName.isEmpty()) {
        ArchArgs chipArgs;
        chipArgs.chipdb = fileName.toStdString();
        currentProj = "";
        disableActions();
        ctx = std::unique_ptr<Context>(new Context(chipArgs));
        actionLoadJSON->setEnabled(true);

        Q_EMIT contextChanged(ctx.get());
    }
}

void MainWindow::load_xdc(std::string filename)
{
    disableActions();
    std::ifstream f(filename);
    ctx->parseXdc(f);
    actionPack->setEnabled(true);
}

void MainWindow::newContext(Context *ctx)
{
    std::string title = "nextpnr-xilinx - " + ctx->getChipName();
    setWindowTitle(title.c_str());
}

void MainWindow::open_xdc()
{
    QString fileName = QFileDialog::getOpenFileName(this, QString("Open XDC"), QString(), QString("*.xdc"));
    if (!fileName.isEmpty()) {
        load_xdc(fileName.toStdString());
    }
}

void MainWindow::save_fasm()
{
    QString fileName = QFileDialog::getSaveFileName(this, QString("Save FASM"), QString(), QString("*.fasm"));
    if (!fileName.isEmpty()) {
        std::string fn = fileName.toStdString();
        disableActions();
        ctx->writeFasm(fn);
        log("Saving FASM successful.\n");
    }
}

void MainWindow::onDisableActions()
{
    actionLoadXDC->setEnabled(false);
    actionSaveFASM->setEnabled(false);
}

void MainWindow::onUpdateActions()
{
    if (ctx->settings.find(ctx->id("pack")) == ctx->settings.end())
        actionLoadXDC->setEnabled(true);
    if (ctx->settings.find(ctx->id("route")) != ctx->settings.end())
        actionSaveFASM->setEnabled(true);
}

NEXTPNR_NAMESPACE_END
