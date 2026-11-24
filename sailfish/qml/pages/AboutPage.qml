import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

Page {
    id: page
    allowedOrientations: Orientation.All

    readonly property string sourceUrl: "https://github.com/JimKnopfIoT/jScan"

    SpaceBackground { }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: page.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("About jScan") }

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Theme.iconSizeExtraLarge
                height: width
                sourceSize.width: width
                sourceSize.height: height
                source: "/usr/share/icons/hicolor/172x172/apps/jscan.png"
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "jScan " + appVersion
                color: "#ff8a00"
                font.pixelSize: Theme.fontSizeLarge
                font.bold: true
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Document scanner for USB sheet-fed scanners (Canon P-215II) "
                           + "and eSCL/AirScan network scanners. Saves PDF, optionally "
                           + "searchable, or PNG.")
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Source code")
                onClicked: Qt.openUrlExternally(page.sourceUrl)
            }

            SectionHeader { text: qsTr("License") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("jScan is free software under the MIT License. "
                           + "It is provided without any warranty.")
            }

            SectionHeader { text: qsTr("Included components") }

            Repeater {
                model: [
                    { name: "SANE backends 1.4.0 (canon_dr)",
                      license: qsTr("GPL-2.0-or-later with the SANE linking exception"),
                      url: "https://gitlab.com/sane-project/backends" },
                    { name: "Tesseract OCR 5.5.1",
                      license: "Apache-2.0",
                      url: "https://github.com/tesseract-ocr/tesseract" },
                    { name: "Leptonica 1.85.0",
                      license: "BSD-2-Clause",
                      url: "https://github.com/DanBloomberg/leptonica" },
                    { name: qsTr("Tesseract language data (deu, eng)"),
                      license: "Apache-2.0",
                      url: "https://github.com/tesseract-ocr/tessdata_fast" }
                ]
                BackgroundItem {
                    width: column.width
                    height: compCol.height + 2 * Theme.paddingSmall
                    onClicked: Qt.openUrlExternally(modelData.url)
                    Column {
                        id: compCol
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        anchors.verticalCenter: parent.verticalCenter
                        Label {
                            width: parent.width
                            text: modelData.name
                            color: highlighted ? Theme.highlightColor : Theme.primaryColor
                            font.pixelSize: Theme.fontSizeSmall
                            truncationMode: TruncationMode.Fade
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.Wrap
                            text: modelData.license
                            color: Theme.secondaryColor
                            font.pixelSize: Theme.fontSizeExtraSmall
                        }
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("The components are included unmodified. "
                           + "License texts: %1").arg("/usr/share/jscan/licenses")
            }
        }

        VerticalScrollDecorator { }
    }
}
