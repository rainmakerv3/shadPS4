// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QComboBox>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include "string"

namespace Ui {
class EditorDialog;
}

class EditorDialog : public QDialog {
    Q_OBJECT 

public: 
    explicit EditorDialog(QWidget* parent = nullptr);
    ~EditorDialog();

protected:

/*
    void closeEvent(QCloseEvent* event) override; // Override close event
    void keyPressEvent(QKeyEvent* event) override; 
*/

private:
    QPlainTextEdit* editor; // Editor widget for the config file
    QFont editorFont;       // To handle the text size
    QString originalConfig; // Starting config string
    std::string gameId;
    std::unique_ptr<Ui::EditorDialog> ui;

    QComboBox* gameComboBox; // Combo box for selecting game configurations

    void loadFile(QString game); // Function to load the config file
    void saveFile(QString game); // Function to save the config file
    void loadInstalledGames();   // Helper to populate gameComboBox
    bool hasUnsavedChanges();    // Checks for unsaved changes

private slots:
    // void onSaveClicked();   // Save button slot
    // void onCancelClicked(); // Slot for handling cancel button
    void onHelpClicked();   // Slot for handling help button
    // void onResetToDefaultClicked();
    // void onGameSelectionChanged(const QString& game); // Slot for game selection changes */
};
