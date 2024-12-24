// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <toml.hpp>

#include "common/config.h"
#include "common/path_util.h"

#include "control_settings.h"
#include "main_window.h"
#include "ui_control_settings.h"

#include "input/input_handler.h"
#include "kbm_config_dialog.h"

#include "common/logging/log.h"

static std::string game_id = "";

ControlSettings::ControlSettings(std::shared_ptr<GameInfoClass> game_info_get, QWidget* parent)
    : QDialog(parent), m_game_info(game_info_get), ui(new Ui::ControlSettings) {

    ui->setupUi(this);

    AddBoxItems();
    SetUIValuestoMappings();

    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) {
        if (button == ui->buttonBox->button(QDialogButtonBox::Save)) {
            SaveControllerConfig(true);
        } else if (button == ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)) {
            SetDefault();
        } else if (button == ui->buttonBox->button(QDialogButtonBox::Apply)) {
            SaveControllerConfig(false);
        }
    });

    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QWidget::close);
    connect(ui->KBMButton, &QPushButton::clicked, this, &ControlSettings::KBMClicked);
    connect(ui->ProfileComboBox, &QComboBox::currentTextChanged, this,
            &ControlSettings::OnProfileChanged);
    connect(ui->LStickButtons, &QCheckBox::stateChanged, this,
            &ControlSettings::LStickButtonsChanged);
    connect(ui->RStickButtons, &QCheckBox::stateChanged, this,
            &ControlSettings::RStickButtonsChanged);
}

void ControlSettings::SaveControllerConfig(bool CloseOnSave) {
    game_id = (ui->ProfileComboBox->currentText().toStdString());
    const auto config_file = Config::GetFoolproofKbmConfigFile(game_id);

    int lineCount = 0;
    int count_axis_left_x = 0;
    int count_axis_left_y = 0;
    int count_axis_right_x = 0;
    int count_axis_right_y = 0;
    std::fstream file(config_file);
    std::string line;
    std::string add;
    std::vector<std::string> lines;
    std::string output_string = "";
    std::string input_string = "";

    while (std::getline(file, line)) {
        lineCount++;

        std::size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            lines.push_back(line);
            continue;
        }

        std::size_t equal_pos = line.find('=');
        if (equal_pos == std::string::npos) {
            lines.push_back(line);
            continue;
        }

        output_string = line.substr(0, equal_pos - 1);
        input_string = line.substr(equal_pos + 2);
        
        if (std::find(controlleroutputs.begin(), controlleroutputs.end(), input_string) !=
            controlleroutputs.end()) {

            if (output_string == "cross") {
                input_string = ui->ABox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "circle") {
                input_string = ui->BBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "square") {
                input_string = ui->XBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "triangle") {
                input_string = ui->YBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "l1") {
                input_string = ui->LBBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "r1") {
                input_string = ui->RBBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "l2") {
                input_string = ui->LTBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "r2") {
                input_string = ui->RTBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "l3") {
                input_string = ui->LClickBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "r3") {
                input_string = ui->RClickBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "options") {
                input_string = ui->StartBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "pad_up") {
                input_string = ui->DpadUpBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "pad_down") {
                input_string = ui->DpadDownBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "pad_left") {
                input_string = ui->DpadLeftBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "pad_right") {
                input_string = ui->DpadRightBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "touchpad") {
                input_string = ui->TouchpadBox->currentText().toStdString();
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "axis_left_x") {
                input_string = ui->LXAxisBox->currentText().toStdString();
                line.erase();
            } else if (output_string == "axis_left_y") {
                input_string = ui->LYAxisBox->currentText().toStdString();
                line.erase();
            } else if (output_string == "axis_right_x") {
                input_string = ui->RXAxisBox->currentText().toStdString();
                line.erase();
            } else if (output_string == "axis_right_y") {
                input_string = ui->RYAxisBox->currentText().toStdString();
                line.erase();
            } else if (output_string == "axis_left_x_minus") {
                line.erase();
            } else if (output_string == "axis_left_x_plus") {
                line.erase();
            } else if (output_string == "axis_left_y_minus") {
                line.erase();
            } else if (output_string == "axis_left_y_plus") {
                line.erase();
            } else if (output_string == "axis_right_x_minus") {
                line.erase();
            } else if (output_string == "axis_right_x_plus") {
                line.erase();
            } else if (output_string == "axis_right_y_minus") {
                line.erase();
            } else if (output_string == "axis_right_y_plus") {
                line.erase();
            } 
        }
            if(!ui->LStickButtons->isChecked()) {
                if (input_string == "axis_left_x") {
                    count_axis_left_x = count_axis_left_x + 1;
                } 
                if (input_string == "axis_left_y") {
                    count_axis_left_y = count_axis_left_y + 1;
                }
            } 
            
            if(!ui->RStickButtons->isChecked()) {
                if (input_string == "axis_right_x") {
                    count_axis_right_x = count_axis_right_x + 1;
                } 
                if (input_string == "axis_right_y") {
                    count_axis_right_y = count_axis_right_y + 1;
                }
            }

            if (count_axis_left_x > 1 | count_axis_left_y > 1 | count_axis_right_x > 1 | count_axis_right_y > 1) {
                QMessageBox::StandardButton nosave;
                nosave = QMessageBox::information(this, "Unable to Save", "Cannot bind axis values more than once");
                return;
            }

            if (std::find(analogoutputs.begin(), analogoutputs.end(), output_string) !=
            analogoutputs.end()) {
                continue;
            }

            if (output_string == "axis_left_buttons") {
                if((ui->LStickButtons->isChecked())) {
                    input_string = "true";
                } else {
                    input_string = "false";
                }
                line.replace(0, 30, output_string + " = " + input_string);
            } else if (output_string == "axis_right_buttons") {
                if((ui->RStickButtons->isChecked())) {
                    input_string = "true";
                } else {
                    input_string = "false";
                }
                line.replace(0, 30, output_string + " = " + input_string);
            }
        lines.push_back(line);
    }

        if(ui->LStickButtons->isChecked()) {
            output_string = "axis_left_x_minus";
            input_string = ui->LStickLeftBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);

            output_string = "axis_left_x_plus";
            input_string = ui->LStickRightBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);

            output_string = "axis_left_y_minus";
            input_string = ui->LStickUpBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);

            output_string = "axis_left_y_plus";
            input_string = ui->LStickDownBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);
        } else {
            output_string = "axis_left_x";
            input_string = ui->LXAxisBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);

            output_string = "axis_left_y";
            input_string = ui->LYAxisBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);
        }

        if(ui->RStickButtons->isChecked()) {
            output_string = "axis_right_x_minus";
            input_string = ui->RStickLeftBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);

            output_string = "axis_right_x_plus";
            input_string = ui->RStickRightBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);

            output_string = "axis_right_y_minus";
            input_string = ui->RStickUpBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);

            output_string = "axis_right_y_plus";
            input_string = ui->RStickDownBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);
        } else {
            output_string = "axis_right_x";
            input_string = ui->RXAxisBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);

            output_string = "axis_right_y";
            input_string = ui->RYAxisBox->currentText().toStdString();
            line = output_string + " = " + input_string;
            lines.push_back(line);
        }

    std::ofstream output_file(config_file);
    for (auto const& line : lines) {
        output_file << line << '\n';
    }
        
    Input::ParseInputConfig(game_id);
    if(CloseOnSave) {
        QWidget::close();
    }   
}

void ControlSettings::SetDefault() {
    ui->ABox->setCurrentIndex(0);
    ui->BBox->setCurrentIndex(1);
    ui->XBox->setCurrentIndex(2);
    ui->YBox->setCurrentIndex(3);
    ui->DpadUpBox->setCurrentIndex(11);
    ui->DpadDownBox->setCurrentIndex(12);
    ui->DpadLeftBox->setCurrentIndex(13);
    ui->DpadRightBox->setCurrentIndex(14);
    ui->LClickBox->setCurrentIndex(8);
    ui->RClickBox->setCurrentIndex(9);
    ui->LBBox->setCurrentIndex(4);
    ui->RBBox->setCurrentIndex(5);
    ui->LTBox->setCurrentIndex(6);
    ui->RTBox->setCurrentIndex(7);
    ui->StartBox->setCurrentIndex(10);
    ui->TouchpadBox->setCurrentIndex(15);

    ui->LStickUpBox->setCurrentIndex(20);
    ui->LStickDownBox->setCurrentIndex(21);
    ui->LStickLeftBox->setCurrentIndex(22);
    ui->LStickRightBox->setCurrentIndex(23);
    ui->RStickUpBox->setCurrentIndex(16);
    ui->RStickDownBox->setCurrentIndex(17);
    ui->RStickLeftBox->setCurrentIndex(18);
    ui->RStickRightBox->setCurrentIndex(19);

    ui->LXAxisBox->setCurrentIndex(0);
    ui->LYAxisBox->setCurrentIndex(1);
    ui->RXAxisBox->setCurrentIndex(2);
    ui->RYAxisBox->setCurrentIndex(3);

    ui->LStickButtons->setChecked(false);
    ui->RStickButtons->setChecked(false);
}

void ControlSettings::AddBoxItems() {
    ui->DpadUpBox->addItems(ButtonInputs);
    ui->DpadDownBox->addItems(ButtonInputs);
    ui->DpadLeftBox->addItems(ButtonInputs);
    ui->DpadRightBox->addItems(ButtonInputs);
    ui->LBBox->addItems(ButtonInputs);
    ui->RBBox->addItems(ButtonInputs);
    ui->LTBox->addItems(ButtonInputs);
    ui->RTBox->addItems(ButtonInputs);
    ui->RClickBox->addItems(ButtonInputs);
    ui->LClickBox->addItems(ButtonInputs);
    ui->StartBox->addItems(ButtonInputs);
    ui->ABox->addItems(ButtonInputs);
    ui->BBox->addItems(ButtonInputs);
    ui->XBox->addItems(ButtonInputs);
    ui->YBox->addItems(ButtonInputs);
    ui->TouchpadBox->addItems(ButtonInputs);

    ui->LStickUpBox->addItems(ButtonInputs);
    ui->LStickDownBox->addItems(ButtonInputs);
    ui->LStickLeftBox->addItems(ButtonInputs);
    ui->LStickRightBox->addItems(ButtonInputs);
    ui->RStickUpBox->addItems(ButtonInputs);
    ui->RStickDownBox->addItems(ButtonInputs);
    ui->RStickLeftBox->addItems(ButtonInputs);
    ui->RStickRightBox->addItems(ButtonInputs);

    ui->LXAxisBox->addItems(StickInputs);
    ui->LYAxisBox->addItems(StickInputs);
    ui->RXAxisBox->addItems(StickInputs);
    ui->RYAxisBox->addItems(StickInputs);

    ui->ProfileComboBox->addItem("Default");
    for (int i = 0; i < m_game_info->m_games.size(); i++) {
        ui->ProfileComboBox->addItem(QString::fromStdString(m_game_info->m_games[i].serial));
    }
    ui->ProfileComboBox->setCurrentText("Default");
    ui->TitleLabel->setText("Default Config");
}

void ControlSettings::SetUIValuestoMappings() {
    game_id = (ui->ProfileComboBox->currentText().toStdString());
    const auto config_file = Config::GetFoolproofKbmConfigFile(game_id);

    int lineCount = 0;
    std::ifstream file(config_file);
    std::string line = "";

    while (std::getline(file, line)) {
        lineCount++;

        line.erase(std::remove(line.begin(), line.end(), ' '), line.end());
        if (line.empty()) {
            continue;
        }

        std::size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }

        std::size_t equal_pos = line.find('=');
        if (equal_pos == std::string::npos) {
            continue;
        }

        std::string output_string = line.substr(0, equal_pos);
        std::string input_string = line.substr(equal_pos + 1);

        if (std::find(controlleroutputs.begin(), controlleroutputs.end(), input_string) !=
            controlleroutputs.end()) {
            if (output_string == "cross") {
                ui->ABox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "circle") {
                ui->BBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "square") {
                ui->XBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "triangle") {
                ui->YBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "l1") {
                ui->LBBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "l2") {
                ui->LTBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "r1") {
                ui->RBBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "r2") {
                ui->RTBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "l3") {
                ui->LClickBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "r3") {
                ui->RClickBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "pad_up") {
                ui->DpadUpBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "pad_down") {
                ui->DpadDownBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "pad_left") {
                ui->DpadLeftBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "pad_right") {
                ui->DpadRightBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "options") {
                ui->StartBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "axis_left_x") {
                ui->LXAxisBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "axis_left_y") {
                ui->LYAxisBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "axis_right_x") {
                ui->RXAxisBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "axis_right_y") {
                ui->RYAxisBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "touchpad") {
                ui->TouchpadBox->setCurrentText(QString::fromStdString(input_string));
            } else if (output_string == "axis_left_x_minus") {
                ui->LStickLeftBox->setCurrentText(QString::fromStdString(input_string)); 
            } else if (output_string == "axis_left_x_plus") {
                ui->LStickRightBox->setCurrentText(QString::fromStdString(input_string)); 
            } else if (output_string == "axis_left_y_minus") {
                ui->LStickUpBox->setCurrentText(QString::fromStdString(input_string)); 
            } else if (output_string == "axis_left_y_plus") {
                ui->LStickDownBox->setCurrentText(QString::fromStdString(input_string)); 
            } else if (output_string == "axis_right_x_minus") {
                ui->RStickLeftBox->setCurrentText(QString::fromStdString(input_string)); 
            } else if (output_string == "axis_right_x_plus") {
                ui->RStickRightBox->setCurrentText(QString::fromStdString(input_string)); 
            } else if (output_string == "axis_right_y_minus") {
                ui->RStickUpBox->setCurrentText(QString::fromStdString(input_string)); 
            } else if (output_string == "axis_right_y_plus") {
                ui->RStickDownBox->setCurrentText(QString::fromStdString(input_string)); 
            }
        }
            if (output_string == "axis_left_buttons") {
                ui->LStickButtons->setChecked((input_string == "true"));
            } else if (output_string == "axis_right_buttons") {
                ui->RStickButtons->setChecked((input_string == "true")); 
            }
    }

    if ((ui->LStickButtons->isChecked())) {
        ui->LXAxisGBox->setVisible(false);
        ui->LYAxisGBox->setVisible(false);
        ui->LStickAxisEnabled->setVisible(true);
        ui->LStickButtonsEnabled->setVisible(false);
        ui->gb_left_stick_up->setVisible(true);
        ui->gb_left_stick_down->setVisible(true);
        ui->gb_left_stick_left->setVisible(true);
        ui->gb_left_stick_right->setVisible(true);
    } else {
        ui->LXAxisGBox->setVisible(true);
        ui->LYAxisGBox->setVisible(true);
        ui->LStickAxisEnabled->setVisible(false);
        ui->LStickButtonsEnabled->setVisible(true);
        ui->gb_left_stick_up->setVisible(false);
        ui->gb_left_stick_down->setVisible(false);
        ui->gb_left_stick_left->setVisible(false);
        ui->gb_left_stick_right->setVisible(false);
    }
  
    if ((ui->RStickButtons->isChecked())) {
        ui->RXAxisGBox->setVisible(false);
        ui->RYAxisGBox->setVisible(false);
        ui->RStickAxisEnabled->setVisible(true);
        ui->RStickButtonsEnabled->setVisible(false);
        ui->gb_right_stick_up->setVisible(true);
        ui->gb_right_stick_down->setVisible(true);
        ui->gb_right_stick_left->setVisible(true);
        ui->gb_right_stick_right->setVisible(true);
    } else {
        ui->RXAxisGBox->setVisible(true);
        ui->RYAxisGBox->setVisible(true);
        ui->RStickAxisEnabled->setVisible(false);
        ui->RStickButtonsEnabled->setVisible(true);
        ui->gb_right_stick_up->setVisible(false);
        ui->gb_right_stick_down->setVisible(false);
        ui->gb_right_stick_left->setVisible(false);
        ui->gb_right_stick_right->setVisible(false);
    }

    file.close();
}

void ControlSettings::GetGameTitle() {
    if (ui->ProfileComboBox->currentText() == "Default") {
        ui->TitleLabel->setText("Default Config");
    } else {
        for (int i = 0; i < m_game_info->m_games.size(); i++) {
            if (m_game_info->m_games[i].serial ==
                ui->ProfileComboBox->currentText().toStdString()) {
                ui->TitleLabel->setText(QString::fromStdString(m_game_info->m_games[i].name));
            }
        }
    }
}

void ControlSettings::OnProfileChanged() {
    GetGameTitle();
    SetUIValuestoMappings();
}

void ControlSettings::KBMClicked() {
    auto KBMWindow = new EditorDialog(this);
    KBMWindow->exec();
}

void ControlSettings::LStickButtonsChanged() {
    if ((ui->LStickButtons->isChecked())) {
        ui->LXAxisGBox->setVisible(false);
        ui->LYAxisGBox->setVisible(false);
        ui->LStickAxisEnabled->setVisible(true);
        ui->LStickButtonsEnabled->setVisible(false);
        ui->gb_left_stick_up->setVisible(true);
        ui->gb_left_stick_down->setVisible(true);
        ui->gb_left_stick_left->setVisible(true);
        ui->gb_left_stick_right->setVisible(true);
        ui->LStickUpBox->setCurrentIndex(11);
        ui->LStickDownBox->setCurrentIndex(12);
        ui->LStickLeftBox->setCurrentIndex(13);
        ui->LStickRightBox->setCurrentIndex(14);
    } else {
        ui->LXAxisGBox->setVisible(true);
        ui->LYAxisGBox->setVisible(true);
        ui->LXAxisBox->setCurrentIndex(0);
        ui->LYAxisBox->setCurrentIndex(1);
        ui->LStickAxisEnabled->setVisible(false);
        ui->LStickButtonsEnabled->setVisible(true);
        ui->gb_left_stick_up->setVisible(false);
        ui->gb_left_stick_down->setVisible(false);
        ui->gb_left_stick_left->setVisible(false);
        ui->gb_left_stick_right->setVisible(false);
    }
}

void ControlSettings::RStickButtonsChanged() {
    if ((ui->RStickButtons->isChecked())) {
        ui->RXAxisGBox->setVisible(false);
        ui->RYAxisGBox->setVisible(false);
        ui->RStickAxisEnabled->setVisible(true);
        ui->RStickButtonsEnabled->setVisible(false);
        ui->gb_right_stick_up->setVisible(true);
        ui->gb_right_stick_down->setVisible(true);
        ui->gb_right_stick_left->setVisible(true);
        ui->gb_right_stick_right->setVisible(true);
        ui->RStickUpBox->setCurrentIndex(3);
        ui->RStickDownBox->setCurrentIndex(0);
        ui->RStickLeftBox->setCurrentIndex(2);
        ui->RStickRightBox->setCurrentIndex(1);
    } else {
        ui->RXAxisGBox->setVisible(true);
        ui->RYAxisGBox->setVisible(true);
        ui->RXAxisBox->setCurrentIndex(2);
        ui->RYAxisBox->setCurrentIndex(3);
        ui->RStickAxisEnabled->setVisible(false);
        ui->RStickButtonsEnabled->setVisible(true);
        ui->gb_right_stick_up->setVisible(false);
        ui->gb_right_stick_down->setVisible(false);
        ui->gb_right_stick_left->setVisible(false);
        ui->gb_right_stick_right->setVisible(false);
    }
}   

ControlSettings::~ControlSettings() {}