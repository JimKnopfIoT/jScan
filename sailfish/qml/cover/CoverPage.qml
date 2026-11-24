import QtQuick 2.6
import Sailfish.Silica 1.0

CoverBackground {
    Column {
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.paddingLarge
        spacing: Theme.paddingSmall

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "jScan"
            color: "#ff8a00"
            font.pixelSize: Theme.fontSizeLarge
            font.bold: true
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: scanner.pageCount === 1 ? qsTr("1 page") : qsTr("%1 pages").arg(scanner.pageCount)
            font.pixelSize: Theme.fontSizeMedium
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            maximumLineCount: 3
            text: scanner.status
            color: Theme.secondaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }
    }

    CoverActionList {
        enabled: scanner.ready || scanner.scanning
        CoverAction {
            iconSource: scanner.scanning ? "image://theme/icon-cover-cancel" : "image://theme/icon-cover-new"
            onTriggered: scanner.scanning ? scanner.cancel() : scanner.scan()
        }
    }
}
