// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <QCheckBox>
#include <QDockWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <cmrc/cmrc.hpp>
#include <common/config.h>
#include "common/path_util.h"
#include "main_window_themes.h"
#include "trophy_viewer.h"

namespace fs = std::filesystem;

CMRC_DECLARE(res);

// true: European format; false: American format
bool useEuropeanDateFormat = true;

void TrophyViewer::updateTrophyInfo() {
    int total = 0;
    int unlocked = 0;

    // Cycles through each tab (table) of the QTabWidget
    for (int i = 0; i < tabWidget->count(); i++) {
        QTableWidget* table = qobject_cast<QTableWidget*>(tabWidget->widget(i));
        if (table) {
            total += table->rowCount();
            for (int row = 0; row < table->rowCount(); ++row) {
                QString cellText;
                // The "Unlocked" column can be a widget or a simple item
                QWidget* widget = table->cellWidget(row, 3);
                if (widget) {
                    // Looks for the QLabel inside the widget (as defined in SetTableItem)
                    QLabel* label = widget->findChild<QLabel*>();
                    if (label) {
                        cellText = label->text();
                    }
                } else {
                    QTableWidgetItem* item = table->item(row, 3);
                    if (item) {
                        cellText = item->text();
                    }
                }
                if (cellText != "")
                    unlocked++;
            }
        }
    }
    int progress = (total > 0) ? (unlocked * 100 / total) : 0;
    trophyInfoLabel->setText(
        QString(tr("Progress") + ": %1% (%2/%3)").arg(progress).arg(unlocked).arg(total));
}

void TrophyViewer::updateTableFilters() {
    bool showEarned = showEarnedCheck->isChecked();
    bool showNotEarned = showNotEarnedCheck->isChecked();
    bool showHidden = showHiddenCheck->isChecked();

    // Cycles through each tab of the QTabWidget
    for (int i = 0; i < tabWidget->count(); ++i) {
        QTableWidget* table = qobject_cast<QTableWidget*>(tabWidget->widget(i));
        if (!table)
            continue;
        for (int row = 0; row < table->rowCount(); ++row) {
            QString timeunlockedText;
            // If Timeunlocked is "", trophy is locked
            QWidget* widget = table->cellWidget(row, 3);
            if (widget) {
                QLabel* label = widget->findChild<QLabel*>();
                if (label)
                    timeunlockedText = label->text();
            } else {
                QTableWidgetItem* item = table->item(row, 3);
                if (item)
                    timeunlockedText = item->text();
            }

            QString hiddenText;
            // Gets the text of the "Hidden" column (index 7)
            QWidget* hiddenWidget = table->cellWidget(row, 6);
            if (hiddenWidget) {
                QLabel* label = hiddenWidget->findChild<QLabel*>();
                if (label)
                    hiddenText = label->text();
            } else {
                QTableWidgetItem* item = table->item(row, 6);
                if (item)
                    hiddenText = item->text();
            }

            bool visible = true;
            if (timeunlockedText != "" && !showEarned)
                visible = false;
            if (timeunlockedText == "" && !showNotEarned)
                visible = false;
            if (hiddenText.toLower() == "yes" && !showHidden)
                visible = false;

            table->setRowHidden(row, !visible);
        }
    }
}

TrophyViewer::TrophyViewer(QString trophyPath, QString gameTrpPath) : QMainWindow() {
    this->setWindowTitle(tr("Trophy Viewer"));
    this->setAttribute(Qt::WA_DeleteOnClose);
    tabWidget = new QTabWidget(this);

    auto lan = Config::getEmulatorLanguage();
    if (lan == "en_US" || lan == "zh_CN" || lan == "zh_TW" || lan == "ja_JP" || lan == "ko_KR" ||
        lan == "lt_LT" || lan == "nb_NO" || lan == "nl_NL") {
        useEuropeanDateFormat = false;
    }

    gameTrpPath_ = gameTrpPath;
    headers << "Trophy"
            << "Name"
            << "Description"
            << "Time Unlocked"
            << "Type"
            << "ID"
            << "Hidden";
    PopulateTrophyWidget(trophyPath);

    QDockWidget* trophyInfoDock = new QDockWidget("", this);
    QWidget* dockWidget = new QWidget(trophyInfoDock);
    QVBoxLayout* dockLayout = new QVBoxLayout(dockWidget);
    dockLayout->setAlignment(Qt::AlignTop);

    trophyInfoLabel = new QLabel(tr("Progress") + ": 0% (0/0)", dockWidget);
    trophyInfoLabel->setStyleSheet(
        "font-weight: bold; font-size: 16px; color: white; background: #333; padding: 5px;");
    dockLayout->addWidget(trophyInfoLabel);

    // Creates QCheckBox to filter trophies
    showEarnedCheck = new QCheckBox(tr("Show Earned Trophies"), dockWidget);
    showNotEarnedCheck = new QCheckBox(tr("Show Not Earned Trophies"), dockWidget);
    showHiddenCheck = new QCheckBox(tr("Show Hidden Trophies"), dockWidget);

    // Defines the initial states (all checked)
    showEarnedCheck->setChecked(true);
    showNotEarnedCheck->setChecked(true);
    showHiddenCheck->setChecked(false);

    // Adds checkboxes to the layout
    dockLayout->addWidget(showEarnedCheck);
    dockLayout->addWidget(showNotEarnedCheck);
    dockLayout->addWidget(showHiddenCheck);

    dockWidget->setLayout(dockLayout);
    trophyInfoDock->setWidget(dockWidget);

    // Adds the dock to the left area
    this->addDockWidget(Qt::LeftDockWidgetArea, trophyInfoDock);

    expandButton = new QPushButton(">>", this);
    expandButton->setGeometry(80, 0, 27, 27);
    expandButton->hide();

    connect(expandButton, &QPushButton::clicked, this, [this, trophyInfoDock] {
        trophyInfoDock->setVisible(true);
        expandButton->hide();
    });

    // Connects checkbox signals to update trophy display
#if (QT_VERSION < QT_VERSION_CHECK(6, 7, 0))
    connect(showEarnedCheck, &QCheckBox::stateChanged, this, &TrophyViewer::updateTableFilters);
    connect(showNotEarnedCheck, &QCheckBox::stateChanged, this, &TrophyViewer::updateTableFilters);
    connect(showHiddenCheck, &QCheckBox::stateChanged, this, &TrophyViewer::updateTableFilters);
#else
    connect(showEarnedCheck, &QCheckBox::checkStateChanged, this,
            &TrophyViewer::updateTableFilters);
    connect(showNotEarnedCheck, &QCheckBox::checkStateChanged, this,
            &TrophyViewer::updateTableFilters);
    connect(showHiddenCheck, &QCheckBox::checkStateChanged, this,
            &TrophyViewer::updateTableFilters);
#endif

    updateTrophyInfo();
    updateTableFilters();

    connect(trophyInfoDock, &QDockWidget::topLevelChanged, this, [this, trophyInfoDock] {
        if (!trophyInfoDock->isVisible()) {
            expandButton->show();
        }
    });

    connect(trophyInfoDock, &QDockWidget::visibilityChanged, this, [this, trophyInfoDock] {
        if (!trophyInfoDock->isVisible()) {
            expandButton->show();
        } else {
            expandButton->hide();
        }
    });
}

void TrophyViewer::onDockClosed() {
    if (!trophyInfoDock->isVisible()) {
        reopenButton->setVisible(true);
    }
}

void TrophyViewer::reopenLeftDock() {
    trophyInfoDock->show();
    reopenButton->setVisible(false);
}

void TrophyViewer::PopulateTrophyWidget(QString title) {
    const auto trophyDir = Common::FS::GetUserPath(Common::FS::PathType::MetaDataDir) /
                           Common::FS::PathFromQString(title) / "TrophyFiles";
    QString trophyDirQt;
    Common::FS::PathToQString(trophyDirQt, trophyDir);

    QDir dir(trophyDirQt);
    if (!dir.exists()) {
        std::filesystem::path path = Common::FS::PathFromQString(gameTrpPath_);
        if (!trp.Extract(path, title.toStdString())) {
            QMessageBox::critical(this, "Trophy Data Extraction Error",
                                  "Unable to extract Trophy data, please ensure you have "
                                  "inputted a trophy key in the settings menu.");
            QWidget::close();
            return;
        }
    }
    QFileInfoList dirList = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (dirList.isEmpty())
        return;

    for (const QFileInfo& dirInfo : dirList) {
        QString tabName = dirInfo.fileName();
        QString trpDir = trophyDirQt + "/" + tabName;

        QString iconsPath = trpDir + "/Icons";
        QDir iconsDir(iconsPath);
        QFileInfoList iconDirList = iconsDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        std::vector<QImage> icons;

        for (const QFileInfo& iconInfo : iconDirList) {
            QImage icon =
                QImage(iconInfo.absoluteFilePath())
                    .scaled(QSize(128, 128), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            icons.push_back(icon);
        }

        QStringList trpId;
        QStringList trpHidden;
        QStringList trpUnlocked;
        QStringList trpType;
        QStringList trpPid;
        QStringList trophyNames;
        QStringList trophyDetails;
        QStringList trpTimeUnlocked;

        QString xmlPath = trpDir + "/Xml/TROP.XML";
        QFile file(xmlPath);
        if (!file.open(QFile::ReadOnly | QFile::Text)) {
            return;
        }

        QXmlStreamReader reader(&file);

        while (!reader.atEnd() && !reader.hasError()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name().toString() == "trophy") {
                trpId.append(reader.attributes().value("id").toString());
                trpHidden.append(reader.attributes().value("hidden").toString());
                trpType.append(reader.attributes().value("ttype").toString());
                trpPid.append(reader.attributes().value("pid").toString());

                if (reader.attributes().hasAttribute("unlockstate")) {
                    if (reader.attributes().value("unlockstate").toString() == "true") {
                        trpUnlocked.append("unlocked");
                    } else {
                        trpUnlocked.append("locked");
                    }
                    if (reader.attributes().hasAttribute("timestamp")) {
                        QString ts = reader.attributes().value("timestamp").toString();
                        if (ts.length() > 10)
                            trpTimeUnlocked.append("unknown");
                        else {
                            bool ok;
                            qint64 timestampInt = ts.toLongLong(&ok);
                            if (ok) {
                                QDateTime dt = QDateTime::fromSecsSinceEpoch(timestampInt);
                                QString format1 =
                                    useEuropeanDateFormat ? "dd/MM/yyyy" : "MM/dd/yyyy";
                                QString format2 = "HH:mm:ss";
                                trpTimeUnlocked.append(dt.toString(format1) + "\n" +
                                                       dt.toString(format2));
                            } else {
                                trpTimeUnlocked.append("unknown");
                            }
                        }
                    } else {
                        trpTimeUnlocked.append("");
                    }
                } else {
                    trpUnlocked.append("locked");
                    trpTimeUnlocked.append("");
                }
            }

            if (reader.name().toString() == "name" && !trpId.isEmpty()) {
                trophyNames.append(reader.readElementText());
            }

            if (reader.name().toString() == "detail" && !trpId.isEmpty()) {
                trophyDetails.append(reader.readElementText());
            }
        }
        QTableWidget* tableWidget = new QTableWidget(this);
        tableWidget->setShowGrid(false);
        tableWidget->setColumnCount(7);
        tableWidget->setHorizontalHeaderLabels(headers);
        tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        tableWidget->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        tableWidget->horizontalHeader()->setStretchLastSection(true);
        tableWidget->verticalHeader()->setVisible(false);
        tableWidget->setRowCount(icons.size());
        tableWidget->setSortingEnabled(true);

        for (int row = 0; auto& icon : icons) {
            QTableWidgetItem* item = new QTableWidgetItem();
            QImage lock_icon =
                QImage(":/images/lock.png")
                    .scaled(QSize(128, 128), Qt::KeepAspectRatio, Qt::SmoothTransformation);

            if (trpUnlocked[row] == "locked") {
                // grey_icon = QImage(icon).convertToFormat(QImage::Format_Grayscale8);
                QPainter painter(&icon);
                painter.drawPixmap(0, 0, 128, 128, QPixmap::fromImage(lock_icon));
                item->setData(Qt::DecorationRole, icon);
            } else {
                item->setData(Qt::DecorationRole, icon);
            }

            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            tableWidget->setItem(row, 0, item);

            const std::string filename = GetTrpType(trpType[row].at(0));
            QTableWidgetItem* typeitem = new QTableWidgetItem();

            const auto CustomTrophy_Dir =
                Common::FS::GetUserPath(Common::FS::PathType::CustomTrophy);
            std::string customPath;

            if (fs::exists(CustomTrophy_Dir / filename)) {
                customPath = (CustomTrophy_Dir / filename).string();
            }

            std::vector<char> imgdata;

            if (!customPath.empty()) {
                std::ifstream file(customPath, std::ios::binary);
                if (file) {
                    imgdata = std::vector<char>(std::istreambuf_iterator<char>(file),
                                                std::istreambuf_iterator<char>());
                }
            } else {
                auto resource = cmrc::res::get_filesystem();
                std::string resourceString = "src/images/" + filename;
                auto file = resource.open(resourceString);
                imgdata = std::vector<char>(file.begin(), file.end());
            }

            QImage type_icon = QImage::fromData(imgdata).scaled(
                QSize(100, 100), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            typeitem->setData(Qt::DecorationRole, type_icon);
            typeitem->setFlags(typeitem->flags() & ~Qt::ItemIsEditable);
            tableWidget->setItem(row, 4, typeitem);

            std::string detailString = trophyDetails[row].toStdString();
            std::size_t newline_pos = 0;
            while ((newline_pos = detailString.find("\n", newline_pos)) != std::string::npos) {
                detailString.replace(newline_pos, 1, " ");
                ++newline_pos;
            }

            if (!trophyNames.isEmpty() && !trophyDetails.isEmpty()) {
                SetTableItem(tableWidget, row, 1, trophyNames[row]);
                SetTableItem(tableWidget, row, 2, QString::fromStdString(detailString));
                SetTableItem(tableWidget, row, 3, trpTimeUnlocked[row]);
                SetTableItem(tableWidget, row, 5, trpId[row]);
                SetTableItem(tableWidget, row, 6, trpHidden[row]);
            }
            tableWidget->verticalHeader()->resizeSection(row, icon.height());
            row++;
        }
        tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        tableWidget->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
        tableWidget->setColumnWidth(1, 200);
        tableWidget->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
        tableWidget->setColumnWidth(2, 450);

        int width = 220;
        for (int i = 0; i < 7; i++) {
            width += tableWidget->horizontalHeader()->sectionSize(i);
        }
        tableWidget->resize(width, 720);

        tabWidget->addTab(tableWidget,
                          tabName.insert(6, " ").replace(0, 1, tabName.at(0).toUpper()));
        this->resize(width, 720);
    }
    this->setCentralWidget(tabWidget);
}

void TrophyViewer::SetTableItem(QTableWidget* parent, int row, int column, QString str) {
    QTableWidgetItem* item = new QTableWidgetItem(str);

    if (column != 0 && column != 1 && column != 2)
        item->setTextAlignment(Qt::AlignCenter);
    item->setFont(QFont("Arial", 12, QFont::Bold));

    Theme theme = static_cast<Theme>(Config::getMainWindowTheme());

    if (theme == Theme::Light) {
        item->setForeground(QBrush(Qt::black));
    } else {
        item->setForeground(QBrush(Qt::white));
    }

    parent->setItem(row, column, item);
}
