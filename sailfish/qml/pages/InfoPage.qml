import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

// Shows a notice the user has to act on, e.g. a missing firmware file.
// The text is selectable so paths can be copied.
Page {
    id: page
    allowedOrientations: Orientation.All

    property string title
    property string text

    SpaceBackground { }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: page.width

            PageHeader { title: page.title }

            TextArea {
                width: parent.width
                readOnly: true
                wrapMode: TextEdit.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.highlightColor
                text: page.text
            }
        }

        VerticalScrollDecorator { }
    }
}
