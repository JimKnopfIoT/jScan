import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import "../components"

// Asks for file name and folder before saving the scanned pages.
Dialog {
    id: dialog
    allowedOrientations: Orientation.All

    property bool png: false
    property string folder: scanner.saveFolder

    readonly property string extension: png ? ".png" : ".pdf"
    readonly property bool exists: scanner.fileExists(folder, nameField.text, png)

    canAccept: nameField.text.trim().length > 0

    onAccepted: scanner.saveAs(png, folder, nameField.text)

    function shortPath(p) {
        return p.replace(/^\/home\/[^\/]+/, "~")
    }

    SpaceBackground { }

    Component {
        id: folderPicker
        FolderPickerPage {
            dialogTitle: qsTr("Save to")
            onSelectedPathChanged: if (selectedPath.length > 0) dialog.folder = selectedPath
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: dialog.width

            DialogHeader {
                acceptText: qsTr("Save")
                title: png ? qsTr("Save as PNG") : qsTr("Save as PDF")
            }

            TextField {
                id: nameField
                width: parent.width
                label: qsTr("File name") + " (" + dialog.extension + ")"
                placeholderText: qsTr("File name")
                text: scanner.defaultFileName()
                inputMethodHints: Qt.ImhNoPredictiveText
                EnterKey.enabled: dialog.canAccept
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.onClicked: dialog.accept()
                Component.onCompleted: selectAll()
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: dialog.exists
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.errorColor
                text: qsTr("A file with this name already exists and will be overwritten.")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: png && scanner.pageCount > 1
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: qsTr("One file per page: %1_01.png, %1_02.png, …").arg(nameField.text.trim())
            }

            ValueButton {
                label: qsTr("Folder")
                value: dialog.shortPath(dialog.folder)
                description: qsTr("Tap to choose another folder")
                onClicked: pageStack.push(folderPicker)
            }

            SectionHeader {
                visible: !png
                text: qsTr("PDF")
            }

            TextSwitch {
                visible: !png
                text: qsTr("Compress PDF")
                description: qsTr("JPEG instead of lossless – much smaller files")
                checked: scanner.compress
                onCheckedChanged: scanner.compress = checked
            }

            TextSwitch {
                visible: !png
                text: qsTr("Searchable (text recognition)")
                description: scanner.ocrAvailable
                             ? qsTr("PDF with invisible text layer (German and English)")
                             : qsTr("Tesseract not found")
                enabled: scanner.ocrAvailable
                checked: scanner.ocr && scanner.ocrAvailable
                onCheckedChanged: if (scanner.ocrAvailable) scanner.ocr = checked
            }
        }

        VerticalScrollDecorator { }
    }
}
