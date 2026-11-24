import QtQuick 2.6
import Sailfish.Silica 1.0

// Neon-bordered touch tile in the house style (like ActionTile in pm5).
BackgroundItem {
    id: tile
    property string title
    property string subtitle
    property color accent: "#ff8a00"

    height: Theme.itemSizeLarge * 1.3

    Rectangle {
        anchors { fill: parent; margins: Theme.paddingSmall }
        radius: Theme.paddingLarge
        opacity: tile.enabled ? 1.0 : 0.4
        color: tile.highlighted ? Qt.rgba(accent.r, accent.g, accent.b, 0.14)
                                : Qt.rgba(1, 1, 1, 0.04)
        border.width: 1
        border.color: Qt.rgba(accent.r, accent.g, accent.b, tile.highlighted ? 0.9 : 0.45)

        Column {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.paddingLarge
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: tile.title
                color: tile.accent
                font.pixelSize: Theme.fontSizeLarge
                font.bold: true
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: text.length > 0
                text: tile.subtitle
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                truncationMode: TruncationMode.Fade
            }
        }
    }
}
