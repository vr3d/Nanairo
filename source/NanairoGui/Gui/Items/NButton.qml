/*!
  \file NButton.qml
  \author Sho Ikeda

  Copyright (c) 2015-2018 Sho Ikeda
  This software is released under the MIT License.
  http://opensource.org/licenses/mit-license.php
  */

import QtQuick 2.11
import QtQuick.Controls 2.4
import QtQuick.Controls.Material 2.4
import "../definitions.js" as Definitions

Button {
  id: button

  Material.elevation: 0

  padding: 0
  leftPadding: 0
  rightPadding: 0
  topPadding: 0
  bottomPadding: 0
  font.pixelSize: Definitions.defaultTextFontSize

  Component.onCompleted: {
    // Initialize the button background
    background.x = 0;
    background.y = 0;
    background.border.width = 1;
    background.border.color = Material.color(Material.Grey);
  }

  onWidthChanged: background.width = width
  onHeightChanged: background.height = height
}
