#pragma once

#include <QtWidgets/QWidget>

#include "ui_generalsettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class GeneralSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit GeneralSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~GeneralSettingsWidget();

private Q_SLOTS:
  void onEnableSpeedLimiterStateChanged();
  void onEmulationSpeedValueChanged(int value);

private:
  Ui::GeneralSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
