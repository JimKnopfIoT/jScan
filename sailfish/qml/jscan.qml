import QtQuick 2.6
import Sailfish.Silica 1.0
import "pages"
import "cover"

ApplicationWindow {
    id: app
    initialPage: Component { MainPage { } }
    cover: Component { CoverPage { } }
    allowedOrientations: defaultAllowedOrientations

    // Notices (e.g. missing scanner firmware) open an info page on top of
    // whatever page is shown; wait until a page transition has finished.
    property var pendingNotice: null

    function showPendingNotice() {
        if (!pendingNotice || pageStack.busy)
            return
        var n = pendingNotice
        pendingNotice = null
        pageStack.push(Qt.resolvedUrl("pages/InfoPage.qml"), { title: n.title, text: n.text })
    }

    Connections {
        target: scanner
        onNotice: { app.pendingNotice = { title: title, text: text }; app.showPendingNotice() }
    }
    Connections {
        target: pageStack
        onBusyChanged: app.showPendingNotice()
    }
}
