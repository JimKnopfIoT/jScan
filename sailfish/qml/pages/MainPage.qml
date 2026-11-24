import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

Page {
    id: page
    allowedOrientations: Orientation.All

    SpaceBackground { }

    Connections {
        target: scanner
        onError: errorBanner.show(message)
        onSaved: savedBanner.show()
    }

    SilicaFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        PullDownMenu {
            MenuItem {
                text: qsTr("About jScan")
                onClicked: pageStack.push(Qt.resolvedUrl("AboutPage.qml"))
            }
            MenuItem {
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"))
            }
            MenuItem {
                text: qsTr("Discard all pages")
                enabled: scanner.pageCount > 0 && !scanner.busy
                onClicked: remorse.execute(qsTr("Discarding pages"), function() { scanner.clearPages() })
            }
            MenuItem {
                text: qsTr("Save as PNG")
                enabled: scanner.pageCount > 0 && !scanner.busy
                onClicked: pageStack.push(Qt.resolvedUrl("SaveDialog.qml"), { png: true })
            }
            MenuItem {
                text: qsTr("Save as PDF")
                enabled: scanner.pageCount > 0 && !scanner.busy
                onClicked: pageStack.push(Qt.resolvedUrl("SaveDialog.qml"), { png: false })
            }
        }

        RemorsePopup { id: remorse }

        Column {
            id: column
            width: page.width
            spacing: Theme.paddingSmall

            PageHeader {
                title: "jScan"
                description: scanner.status
            }

            // --- page view: pinch or double-tap to zoom, drag to pan -----------
            Item {
                id: viewBox
                width: parent.width - 2 * Theme.horizontalPageMargin
                x: Theme.horizontalPageMargin
                height: page.isPortrait ? page.height * 0.48 : page.height * 0.7

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.paddingMedium
                    color: Qt.rgba(0, 0, 0, 0.35)
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, 0.08)
                }

                Flickable {
                    id: zoomer
                    property real zoom: 1.0
                    anchors.fill: parent
                    anchors.margins: Theme.paddingSmall
                    clip: true
                    contentWidth: width * zoom
                    contentHeight: height * zoom
                    interactive: zoom > 1.0
                    boundsBehavior: Flickable.StopAtBounds

                    function reset() { zoom = 1.0; contentX = 0; contentY = 0 }

                    PinchArea {
                        width: zoomer.contentWidth
                        height: zoomer.contentHeight
                        property real startZoom: 1.0
                        onPinchStarted: startZoom = zoomer.zoom
                        onPinchUpdated: {
                            var z = Math.max(1.0, Math.min(8.0, startZoom * pinch.scale))
                            var cx = (zoomer.contentX + pinch.center.x) / zoomer.zoom
                            var cy = (zoomer.contentY + pinch.center.y) / zoomer.zoom
                            zoomer.zoom = z
                            zoomer.contentX = Math.max(0, Math.min(zoomer.contentWidth - zoomer.width, cx * z - pinch.center.x))
                            zoomer.contentY = Math.max(0, Math.min(zoomer.contentHeight - zoomer.height, cy * z - pinch.center.y))
                        }

                        Image {
                            id: pageImage
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false
                            smooth: true
                            // live while scanning, otherwise the selected page
                            source: scanner.scanning && scanner.liveImage.length > 0
                                    ? scanner.liveImage : scanner.currentImage
                            sourceSize.width: 1800
                        }

                        MouseArea {
                            anchors.fill: parent
                            onDoubleClicked: {
                                if (zoomer.zoom > 1.0) {
                                    zoomer.reset()
                                } else {
                                    var cx = mouse.x, cy = mouse.y
                                    zoomer.zoom = 3.0
                                    zoomer.contentX = Math.max(0, Math.min(zoomer.contentWidth - zoomer.width, cx * 3 - zoomer.width / 2))
                                    zoomer.contentY = Math.max(0, Math.min(zoomer.contentHeight - zoomer.height, cy * 3 - zoomer.height / 2))
                                }
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 2 * Theme.paddingLarge
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    visible: pageImage.status !== Image.Ready
                    color: Theme.secondaryColor
                    text: scanner.busy && !scanner.scanning ? ""
                          : scanner.feeder ? scanner.feedHint
                                           : qsTr("Place the document and tap “Scan”.")
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    size: BusyIndicatorSize.Medium
                    running: scanner.busy && pageImage.status !== Image.Ready
                }

                Label {
                    anchors { top: parent.top; right: parent.right; margins: Theme.paddingMedium }
                    visible: zoomer.zoom > 1.0
                    text: Math.round(zoomer.zoom * 100) + " %"
                    color: "#ff8a00"
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
            }

            // --- pages strip ------------------------------------------------------
            SilicaListView {
                id: strip
                width: parent.width
                height: scanner.pageCount > 0 ? Theme.itemSizeHuge * 1.25 : 0
                visible: scanner.pageCount > 0
                orientation: ListView.Horizontal
                spacing: Theme.paddingSmall
                leftMargin: Theme.horizontalPageMargin
                rightMargin: Theme.horizontalPageMargin
                clip: true
                model: scanner.thumbs
                currentIndex: scanner.currentPage
                onCountChanged: positionViewAtEnd()

                delegate: ListItem {
                    id: thumb
                    width: Theme.itemSizeHuge * 0.78
                    contentHeight: strip.height
                    onClicked: { scanner.currentPage = index; zoomer.reset() }

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: Theme.paddingSmall
                        color: "transparent"
                        border.width: index === scanner.currentPage ? 2 : 1
                        border.color: index === scanner.currentPage ? "#ff8a00" : Qt.rgba(1, 1, 1, 0.15)
                    }
                    Image {
                        anchors { fill: parent; margins: Theme.paddingSmall; bottomMargin: Theme.paddingLarge * 1.4 }
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                        source: modelData
                        sourceSize.height: strip.height
                    }
                    Label {
                        anchors { bottom: parent.bottom; bottomMargin: Theme.paddingSmall; horizontalCenter: parent.horizontalCenter }
                        text: (index + 1)
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: index === scanner.currentPage ? "#ff8a00" : Theme.secondaryColor
                    }

                    menu: ContextMenu {
                        MenuItem { text: qsTr("Rotate left"); onClicked: scanner.rotatePage(index, -90) }
                        MenuItem { text: qsTr("Rotate right"); onClicked: scanner.rotatePage(index, 90) }
                        MenuItem { text: qsTr("Move forward"); visible: index > 0; onClicked: scanner.movePage(index, index - 1) }
                        MenuItem { text: qsTr("Move back"); visible: index < scanner.pageCount - 1; onClicked: scanner.movePage(index, index + 1) }
                        MenuItem { text: qsTr("Delete"); onClicked: thumb.remorseDelete(function() { scanner.removePage(index) }) }
                    }
                }
            }

            // --- adjustments (current page and next scans) -------------------------
            ComboBox {
                label: qsTr("Color")
                currentIndex: scanner.colorMode
                menu: ContextMenu {
                    MenuItem { text: qsTr("Color") }
                    MenuItem { text: qsTr("Grayscale") }
                    MenuItem { text: qsTr("Black and white") }
                }
                onCurrentIndexChanged: scanner.colorMode = currentIndex
            }

            Slider {
                width: parent.width
                label: qsTr("Contrast")
                minimumValue: -50
                maximumValue: 50
                stepSize: 1
                value: scanner.contrast
                valueText: value > 0 ? "+" + value : value
                onValueChanged: if (Math.round(value) !== scanner.contrast) scanner.contrast = Math.round(value)
            }

            Slider {
                width: parent.width
                label: qsTr("Smooth background")
                minimumValue: 0
                maximumValue: 100
                stepSize: 1
                value: scanner.flatten
                valueText: value === 0 ? qsTr("off") : value
                onValueChanged: if (Math.round(value) !== scanner.flatten) scanner.flatten = Math.round(value)
            }

            TextSwitch {
                text: qsTr("Deskew")
                checked: scanner.deskew
                onCheckedChanged: scanner.deskew = checked
            }

            ScanTile {
                width: parent.width - 2 * Theme.horizontalPageMargin
                x: Theme.horizontalPageMargin
                enabled: scanner.scanning || scanner.ready
                accent: scanner.scanning ? "#ff4d4d" : "#ff8a00"
                title: scanner.scanning ? qsTr("Cancel") : qsTr("Scan")
                subtitle: scanner.devices.length > 0 && scanner.deviceIndex >= 0
                          ? scanner.devices[scanner.deviceIndex] : qsTr("No scanner")
                onClicked: scanner.scanning ? scanner.cancel() : scanner.scan()
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: qsTr("The sliders apply to the selected page and to the next scans. "
                           + "Pull down to save.")
            }
        }

        VerticalScrollDecorator { }
    }

    // --- banners ---------------------------------------------------------------
    Rectangle {
        id: errorBanner
        property alias text: errorLabel.text
        function show(msg) { text = msg; opacity = 1.0; errorTimer.restart() }
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: errorLabel.height + 2 * Theme.paddingLarge
        color: Qt.rgba(0.1, 0.02, 0.02, 0.92)
        opacity: 0.0
        visible: opacity > 0
        Behavior on opacity { FadeAnimation { } }
        Label {
            id: errorLabel
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.horizontalPageMargin
            wrapMode: Text.Wrap
            color: "#ff4d4d"
        }
        Timer { id: errorTimer; interval: 6000; onTriggered: errorBanner.opacity = 0.0 }
        MouseArea { anchors.fill: parent; onClicked: errorBanner.opacity = 0.0 }
    }

    Rectangle {
        id: savedBanner
        function show() { opacity = 1.0; savedTimer.restart() }
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: savedLabel.height + 2 * Theme.paddingLarge
        color: Qt.rgba(0.02, 0.1, 0.05, 0.92)
        opacity: 0.0
        visible: opacity > 0
        Behavior on opacity { FadeAnimation { } }
        Label {
            id: savedLabel
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.horizontalPageMargin
            wrapMode: Text.Wrap
            color: "#28c76f"
            text: scanner.status + "\n" + qsTr("Folder: %1").arg(scanner.saveFolder)
        }
        Timer { id: savedTimer; interval: 5000; onTriggered: savedBanner.opacity = 0.0 }
        MouseArea { anchors.fill: parent; onClicked: savedBanner.opacity = 0.0 }
    }
}
