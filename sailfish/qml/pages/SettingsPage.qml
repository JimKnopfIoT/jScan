import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

Page {
    id: page
    allowedOrientations: Orientation.All

    SpaceBackground { }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        PullDownMenu {
            MenuItem {
                text: qsTr("Search for scanners again")
                enabled: !scanner.busy
                onClicked: scanner.refreshDevices()
            }
        }

        Column {
            id: column
            width: page.width

            PageHeader { title: qsTr("Settings"); description: scanner.status }

            SectionHeader { text: qsTr("Scanner") }

            ComboBox {
                label: qsTr("Device")
                enabled: !scanner.busy && scanner.devices.length > 0
                currentIndex: scanner.deviceIndex
                menu: ContextMenu {
                    Repeater {
                        model: scanner.devices
                        MenuItem { text: modelData }
                    }
                }
                onCurrentIndexChanged: if (currentIndex >= 0) scanner.deviceIndex = currentIndex
            }

            ComboBox {
                label: qsTr("Source")
                enabled: scanner.sources.length > 1
                currentIndex: scanner.sourceIndex
                menu: ContextMenu {
                    Repeater {
                        model: scanner.sources
                        MenuItem { text: modelData }
                    }
                }
                onCurrentIndexChanged: if (currentIndex >= 0) scanner.sourceIndex = currentIndex
            }

            ComboBox {
                label: qsTr("Resolution")
                enabled: scanner.resolutions.length > 0
                currentIndex: scanner.dpiIndex
                menu: ContextMenu {
                    Repeater {
                        model: scanner.resolutions
                        MenuItem { text: modelData + " dpi" }
                    }
                }
                onCurrentIndexChanged: if (currentIndex >= 0) scanner.dpiIndex = currentIndex
            }

            TextSwitch {
                text: qsTr("Remove blank pages")
                description: qsTr("Feeder only")
                checked: scanner.skipBlank
                onCheckedChanged: scanner.skipBlank = checked
            }

            SectionHeader { text: qsTr("Network scanner") }

            TextField {
                width: parent.width
                label: qsTr("IP address (eSCL/AirScan)")
                placeholderText: qsTr("e.g. 192.168.1.20")
                text: scanner.networkHost
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.onClicked: { scanner.networkHost = text; focus = false; scanner.refreshDevices() }
            }

            SectionHeader { text: qsTr("Saving") }

            TextSwitch {
                text: qsTr("Compress PDF")
                description: qsTr("JPEG instead of lossless – much smaller files")
                checked: scanner.compress
                onCheckedChanged: scanner.compress = checked
            }

            TextSwitch {
                text: qsTr("Searchable (text recognition)")
                description: scanner.ocrAvailable
                             ? qsTr("PDF with invisible text layer (German and English)")
                             : qsTr("Tesseract not found")
                enabled: scanner.ocrAvailable
                checked: scanner.ocr && scanner.ocrAvailable
                onCheckedChanged: if (scanner.ocrAvailable) scanner.ocr = checked
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: qsTr("Storage folder: %1").arg(scanner.saveFolder)
            }
        }

        VerticalScrollDecorator { }
    }
}
