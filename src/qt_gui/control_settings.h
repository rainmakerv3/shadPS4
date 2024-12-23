// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include <QPushButton>

#include "game_info.h"

namespace Ui {
class ControlSettings;
}

class ControlSettings : public QDialog {
    Q_OBJECT
public:
    explicit ControlSettings(std::shared_ptr<GameInfoClass> game_info_get,
                             QWidget* parent = nullptr);
    ~ControlSettings();
    const std::vector<std::string> controlleroutputs = {
        "cross",        "circle",    "square",      "triangle",    "l1",
        "r1",           "l2",        "r2",          "l3",

        "r3",           "options",   "pad_up",

        "pad_down",

        "pad_left",     "pad_right", "axis_left_x", "axis_left_y", "axis_right_x",
        "axis_right_y", "back", "axis_left_x_minus", "axis_left_x_plus", 
        "axis_left_y_minus", "axis_left_y_plus", "axis_right_x_minus", "axis_right_x_plus", 
        "axis_right_y_minus", "axis_right_y_plus"};

    const std::vector<std::string> analogoutputs = {
        "axis_left_x", "axis_left_y", "axis_right_x",
        "axis_right_y", "back", "axis_left_x_minus", "axis_left_x_plus", 
        "axis_left_y_minus", "axis_left_y_plus", "axis_right_x_minus", "axis_right_x_plus", 
        "axis_right_y_minus", "axis_right_y_plus"};
    
    const QStringList ButtonInputs = {"cross",        "circle",       "square",      "triangle",
                                "l1",           "r1",           "l2",          "r2",
                                "l3",

                                "r3",           "options",      "pad_up",

                                "pad_down",

                                "pad_left",     "pad_right",    "back"};

    const QStringList StickInputs = {"axis_left_x", "axis_left_y",
                                "axis_right_x", "axis_right_y"};

private Q_SLOTS:
    void SaveControllerConfig(bool CloseOnSave);
    void SetDefault();
    void AddBoxItems();
    void SetUIValuestoMappings();
    void GetGameTitle();
    void OnProfileChanged();
    void KBMClicked();
    void LStickButtonsChanged();
    void RStickButtonsChanged();

private:
    std::unique_ptr<Ui::ControlSettings> ui;
    std::shared_ptr<GameInfoClass> m_game_info;
};
