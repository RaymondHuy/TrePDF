pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "ios"

Control {
    id: root
    objectName: "readerControlBar"

    required property AppTheme theme
    property bool documentLoaded: false
    property int navigationPage: 0
    property int pageCount: 0
    property bool canNavigateBackward: navigationPage > 0
    property bool canNavigateForward:
        documentLoaded && navigationPage + 1 < pageCount
    property real zoom: 1.0
    property int pageDisplayMode: ReaderModes.ContinuousMode
    property var allowedPageDisplayModes: [
        ReaderModes.SinglePageMode, ReaderModes.ContinuousMode,
        ReaderModes.FacingMode, ReaderModes.ContinuousFacingMode
    ]
    property bool viewReady: documentLoaded
    property var viewContextOwner: null
    property int viewContextPane: 0

    signal pageRequested(int page)
    signal pageStepRequested(int direction)
    signal zoomRequested(real value)
    signal pageDisplayModeRequested(int mode)
    signal viewRestoreRequested(int page, real zoom)

    property var _viewHistories: []
    property bool _canRestoreBackward: false
    property bool _canRestoreForward: false
    property var _observedViewState: undefined
    property bool _contextActivationPending: false
    property int _contextActivationGeneration: 0
    property bool _observationPending: false
    property int _observationGeneration: 0
    property bool _viewRestorePending: false
    property int _viewRestoreGeneration: 0
    property int _sliderStartPage: 0
    property real _sliderStartZoom: 1.0
    property bool _sliderGestureChanged: false

    readonly property var zoomPresets: [
        10, 25, 50, 75, 100, 125, 150, 200, 300, 400, 500
    ]
    readonly property int _maximumHistoryContexts: 64
    readonly property bool _interactionReady:
        documentLoaded && viewReady && !_contextActivationPending

    readonly property real controlSize: Qt.platform.os === "ios" ? 40 : 28
    implicitHeight: (Qt.platform.os === "ios" ? 48 : 38) + bottomPadding - 4
    leftPadding: 6
    rightPadding: 6
    topPadding: 4
    bottomPadding: 4

    function _boundedPage(page) {
        return Math.max(0, Math.min(Math.max(0, pageCount - 1), page))
    }

    function _boundedZoom(value) {
        return Math.max(0.1, Math.min(5.0, value))
    }

    function _viewState(page, zoomValue) {
        return {
            "page": _boundedPage(page),
            "zoom": _boundedZoom(zoomValue)
        }
    }

    function _sameViewState(left, right) {
        return left !== undefined && right !== undefined
            && left.page === right.page
            && Math.abs(left.zoom - right.zoom) < 0.0001
    }

    function _historyRecord() {
        const owner = viewContextOwner !== null ? viewContextOwner : root
        for (let index = 0; index < _viewHistories.length; ++index) {
            const record = _viewHistories[index]
            if (record.owner === owner && record.pane === viewContextPane) {
                if (index + 1 < _viewHistories.length) {
                    _viewHistories.splice(index, 1)
                    _viewHistories.push(record)
                }
                return record
            }
        }

        while (_viewHistories.length >= _maximumHistoryContexts)
            _viewHistories.shift()
        const record = {
            "owner": owner,
            "pane": viewContextPane,
            "pageCount": pageCount,
            "history": { "entries": [], "index": -1 }
        }
        _viewHistories.push(record)
        return record
    }

    function _ensureHistory() {
        const record = _historyRecord()
        const history = record.history
        if (record.pageCount !== pageCount) {
            record.pageCount = pageCount
            history.entries = []
            history.index = -1
            _observedViewState = undefined
        }
        if (history.entries.length === 0 && documentLoaded) {
            history.entries.push(_viewState(navigationPage, zoom))
            history.index = 0
        }
        return history
    }

    function _refreshHistoryAvailability() {
        if (!documentLoaded || !viewReady || _contextActivationPending) {
            _canRestoreBackward = false
            _canRestoreForward = false
            return
        }

        const history = _ensureHistory()
        _canRestoreBackward = history.index > 0
        _canRestoreForward = history.index >= 0
            && history.index + 1 < history.entries.length
    }

    function _appendHistoryState(history, state) {
        history.entries.push(state)
        history.index = history.entries.length - 1
        if (history.entries.length > 64) {
            const overflow = history.entries.length - 64
            history.entries.splice(0, overflow)
            history.index -= overflow
        }
    }

    function _recordViewTransition(startPage, startZoom,
                                   targetPage, targetZoom) {
        if (!documentLoaded || !viewReady)
            return

        const history = _ensureHistory()
        const start = _viewState(startPage, startZoom)
        const target = _viewState(targetPage, targetZoom)
        if (history.index >= 0
                && _sameViewState(history.entries[history.index], target)) {
            _refreshHistoryAvailability()
            return
        }

        if (history.index + 1 < history.entries.length)
            history.entries.splice(history.index + 1)

        if (history.index < 0
                || !_sameViewState(history.entries[history.index], start)) {
            _appendHistoryState(history, start)
        }
        if (!_sameViewState(start, target))
            _appendHistoryState(history, target)

        _refreshHistoryAvailability()
    }

    function _scheduleContextActivation() {
        const generation = ++_contextActivationGeneration
        ++_observationGeneration
        _observationPending = false
        ++_viewRestoreGeneration
        _viewRestorePending = false
        _observedViewState = undefined
        _canRestoreBackward = false
        _canRestoreForward = false
        if (!documentLoaded || !viewReady) {
            _contextActivationPending = false
            _syncPageField(false)
            return
        }

        _contextActivationPending = true
        const owner = viewContextOwner
        const pane = viewContextPane
        Qt.callLater(function() {
            if (generation !== root._contextActivationGeneration
                    || owner !== root.viewContextOwner
                    || pane !== root.viewContextPane) {
                return
            }

            root._contextActivationPending = false
            root._syncPageField(false)
            if (root.documentLoaded) {
                const history = root._ensureHistory()
                const current = root._viewState(
                    root.navigationPage, root.zoom)
                if (history.index >= 0
                        && !root._sameViewState(
                            history.entries[history.index], current)) {
                    if (history.index + 1 < history.entries.length)
                        history.entries.splice(history.index + 1)
                    root._appendHistoryState(history, current)
                }
                root._observedViewState = current
            }
            root._refreshHistoryAvailability()
        })
    }

    function _scheduleViewObservation() {
        const generation = ++_observationGeneration
        _observationPending = true
        const owner = viewContextOwner
        const pane = viewContextPane
        Qt.callLater(function() {
            if (generation !== root._observationGeneration
                    || owner !== root.viewContextOwner
                    || pane !== root.viewContextPane) {
                return
            }
            root._observationPending = false
            root._observeCurrentView()
        })
    }

    function _observeCurrentView() {
        if (!documentLoaded || !viewReady || _contextActivationPending) {
            _observedViewState = undefined
            _refreshHistoryAvailability()
            return
        }

        const current = _viewState(navigationPage, zoom)
        if (_viewRestorePending || readerZoomSlider.pressed) {
            _observedViewState = current
            return
        }

        const history = _ensureHistory()
        if (history.index >= 0
                && !_sameViewState(history.entries[history.index], current)) {
            const start = _observedViewState !== undefined
                ? _observedViewState
                : history.entries[history.index]
            _recordViewTransition(
                start.page, start.zoom, current.page, current.zoom)
        }
        _observedViewState = current
    }

    function _requestPage(targetPage, stepDirection) {
        if (!_interactionReady || pageCount <= 0)
            return

        const target = _boundedPage(targetPage)
        if (target === navigationPage)
            return

        if (stepDirection === 0)
            pageRequested(target)
        else
            pageStepRequested(stepDirection)
    }

    function _requestZoom(targetZoom) {
        if (!_interactionReady)
            return

        const target = _boundedZoom(targetZoom)
        if (Math.abs(target - zoom) < 0.0001)
            return

        zoomRequested(target)
    }

    function _steppedZoom(direction) {
        const percent = _boundedZoom(zoom) * 100.0
        if (direction < 0) {
            for (let index = zoomPresets.length - 1; index >= 0; --index) {
                if (zoomPresets[index] < percent - 0.01)
                    return zoomPresets[index] / 100.0
            }
            return 0.1
        }

        for (let index = 0; index < zoomPresets.length; ++index) {
            if (zoomPresets[index] > percent + 0.01)
                return zoomPresets[index] / 100.0
        }
        return 5.0
    }

    function _zoomToSlider(zoomValue) {
        const minimum = Math.log(0.1)
        const span = Math.log(5.0) - minimum
        return (Math.log(_boundedZoom(zoomValue)) - minimum) / span
    }

    function _sliderToZoom(sliderValue) {
        const minimum = Math.log(0.1)
        const span = Math.log(5.0) - minimum
        return _boundedZoom(Math.exp(minimum + sliderValue * span))
    }

    function _zoomPresetIndex(zoomValue) {
        const percent = _boundedZoom(zoomValue) * 100.0
        for (let index = 0; index < zoomPresets.length; ++index) {
            if (Math.abs(zoomPresets[index] - percent) < 0.01)
                return index
        }
        return -1
    }

    function _zoomText(zoomValue) {
        const percent = Math.round(
            _boundedZoom(zoomValue) * 10000.0) / 100.0
        const tenths = Math.round(percent * 10.0) / 10.0
        const fractionDigits = Math.abs(percent - Math.round(percent)) < 0.001
            ? 0
            : Math.abs(percent - tenths) < 0.001
                ? 1
                : 2
        const percentText = percent.toLocaleString(
            Qt.locale(), "f", fractionDigits)
        //% "%1%"
        const pattern = qsTrId("PercentValue")
        return pattern === "PercentValue"
            ? percentText + "%"
            : pattern.arg(percentText)
    }

    function _pageCountText(count) {
        const boundedCount = Math.max(0, count)
        //% "/ %1"
        const pattern = qsTrId("PageCountSuffix")
        return pattern === "PageCountSuffix"
            ? "/ " + boundedCount
            : pattern.arg(boundedCount)
    }

    function _commitPageField() {
        if (readerPageField.acceptableInput) {
            const requestedPage = Number.fromLocaleString(
                Qt.locale(), readerPageField.text)
            _requestPage(Math.round(requestedPage) - 1, 0)
        }
        _syncPageField(true)
    }

    function _syncPageField(force) {
        if (force || !readerPageField.activeFocus)
            readerPageField.text = documentLoaded && pageCount > 0
                ? String(_boundedPage(navigationPage) + 1)
                : ""
    }

    function _restoreView(direction) {
        if (!_interactionReady)
            return

        const history = _ensureHistory()
        const targetIndex = history.index + direction
        if (targetIndex < 0 || targetIndex >= history.entries.length)
            return

        history.index = targetIndex
        const state = history.entries[targetIndex]
        const generation = ++_viewRestoreGeneration
        _viewRestorePending = true
        _observedViewState = state
        _refreshHistoryAvailability()
        viewRestoreRequested(_boundedPage(state.page), _boundedZoom(state.zoom))
        Qt.callLater(function() {
            if (generation !== root._viewRestoreGeneration)
                return

            root._viewRestorePending = false
            if (root.documentLoaded && root.viewReady
                    && !root._contextActivationPending) {
                const current = root._viewState(
                    root.navigationPage, root.zoom)
                const activeHistory = root._ensureHistory()
                if (activeHistory === history
                        && history.index >= 0
                        && !root._sameViewState(
                            history.entries[history.index], current)) {
                    history.entries[history.index] = current
                }
                root._observedViewState = current
            }
            root._refreshHistoryAvailability()
        })
    }

    onViewContextOwnerChanged: _scheduleContextActivation()
    onViewContextPaneChanged: _scheduleContextActivation()
    onDocumentLoadedChanged: _scheduleContextActivation()
    onViewReadyChanged: _scheduleContextActivation()
    onNavigationPageChanged: {
        _syncPageField(false)
        _scheduleViewObservation()
    }
    onPageCountChanged: {
        _syncPageField(false)
        _scheduleViewObservation()
    }
    onZoomChanged: _scheduleViewObservation()
    Component.onCompleted: {
        _syncPageField(true)
        _scheduleContextActivation()
    }

    component BarIconButton: ToolButton {
        id: iconButton

        required property AppTheme theme
        required property url iconSource
        required property string accessibleText
        property bool selected: false

        implicitWidth: 28
        implicitHeight: 28
        padding: 5
        focusPolicy: Qt.StrongFocus
        display: AbstractButton.IconOnly
        checked: selected
        autoExclusive: checkable
        Accessible.name: accessibleText
        icon.source: iconSource
        icon.width: 18
        icon.height: 18
        icon.color: !enabled
            ? "#aaa8a6"
            : selected
                ? theme.foxitDarkOrange
                : "#4a4b50"

        background: Rectangle {
            radius: 3
            color: iconButton.down
                ? "#f5dfcf"
                : iconButton.hovered || iconButton.visualFocus
                        || iconButton.selected
                    ? iconButton.theme.foxitPaleOrange
                    : "transparent"
            border.width: iconButton.selected ? 1 : 0
            border.color: iconButton.theme.foxitBorderOrange
        }

        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: accessibleText
    }

    component BarGlyphButton: ToolButton {
        id: glyphButton

        required property AppTheme theme
        required property string accessibleText

        implicitWidth: 28
        implicitHeight: 28
        padding: 3
        focusPolicy: Qt.StrongFocus
        font.pixelSize: 17
        Accessible.name: accessibleText

        contentItem: Label {
            text: glyphButton.text
            color: glyphButton.enabled
                ? glyphButton.theme.ribbonTextColor
                : "#aaa8a6"
            font: glyphButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            Accessible.ignored: true
        }

        background: Rectangle {
            radius: 3
            color: glyphButton.down
                ? "#f5dfcf"
                : glyphButton.hovered || glyphButton.visualFocus
                    ? glyphButton.theme.foxitPaleOrange
                    : "transparent"
        }

        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: accessibleText
    }

    background: Rectangle {
        color: root.theme.ribbonSurface
        border.width: 1
        border.color: root.theme.ribbonBorder
    }

    contentItem: HorizontalControlViewport {
        id: controlViewport
        contentImplicitWidth: controlRow.implicitWidth
        contentImplicitHeight: controlRow.implicitHeight
        content: [controlRow]
    }

    readonly property RowLayout _controlRow: RowLayout {
        id: controlRow
        width: controlViewport.contentWidth
        height: controlViewport.height
        spacing: 2

        BarGlyphButton {
            id: readerFirstPageButton
            objectName: "readerFirstPageButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady && root.navigationPage > 0
            text: "\u00ab"
            //% "First Page"
            accessibleText: qsTrId("FirstPage")
            onClicked: root._requestPage(0, 0)
        }

        BarIconButton {
            id: readerPreviousPageButton
            objectName: "readerPreviousPageButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady && root.canNavigateBackward
            iconSource: "../assets/icons/chevron-left.svg"
            //% "Previous Page"
            accessibleText: qsTrId("PreviousPage")
            onClicked: root._requestPage(root.navigationPage - 1, -1)
        }

        TextField {
            id: readerPageField
            objectName: "readerPageField"

            property bool acceptedSinceFocus: false

            Layout.preferredWidth: 44
            Layout.preferredHeight: 26
            enabled: root._interactionReady && root.pageCount > 0
            selectByMouse: true
            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
            font.pixelSize: 12
            leftPadding: 4
            rightPadding: 4
            topPadding: 2
            bottomPadding: 2
            color: root.theme.ribbonTextColor
            //% "Page"
            Accessible.name: qsTrId("Page")
            validator: IntValidator {
                bottom: 1
                top: Math.max(1, root.pageCount)
            }
            onActiveFocusChanged: {
                if (activeFocus) {
                    acceptedSinceFocus = false
                    selectAll()
                }
            }
            onTextEdited: acceptedSinceFocus = false
            onAccepted: {
                acceptedSinceFocus = true
                root._commitPageField()
            }
            onEditingFinished: {
                if (!acceptedSinceFocus)
                    root._commitPageField()
                acceptedSinceFocus = false
            }

            background: Rectangle {
                radius: 3
                color: readerPageField.enabled ? "#ffffff" : "#f4f2f0"
                border.width: 1
                border.color: readerPageField.activeFocus
                    ? root.theme.foxitBorderOrange
                    : root.theme.ribbonBorder
            }
        }

        Label {
            id: readerPageCountLabel
            objectName: "readerPageCountLabel"

            Layout.minimumWidth: 34
            Layout.preferredHeight: root.controlSize
            text: root._pageCountText(root.pageCount)
            color: root.documentLoaded
                ? root.theme.ribbonTextColor
                : "#aaa8a6"
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
        }

        BarIconButton {
            id: readerNextPageButton
            objectName: "readerNextPageButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady && root.canNavigateForward
            iconSource: "../assets/icons/chevron-right.svg"
            //% "Next Page"
            accessibleText: qsTrId("NextPage")
            onClicked: root._requestPage(root.navigationPage + 1, 1)
        }

        BarGlyphButton {
            id: readerLastPageButton
            objectName: "readerLastPageButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady
                && root.navigationPage + 1 < root.pageCount
            text: "\u00bb"
            //% "Last Page"
            accessibleText: qsTrId("LastPage")
            onClicked: root._requestPage(root.pageCount - 1, 0)
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 20
            color: root.theme.ribbonBorder
            Accessible.ignored: true
        }

        BarIconButton {
            id: readerPreviousViewButton
            objectName: "readerPreviousViewButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._canRestoreBackward
            iconSource: "../assets/icons/undo-2.svg"
            //% "Previous View"
            accessibleText: qsTrId("PreviousView")
            onClicked: root._restoreView(-1)
        }

        BarIconButton {
            id: readerNextViewButton
            objectName: "readerNextViewButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._canRestoreForward
            iconSource: "../assets/icons/redo-2.svg"
            //% "Next View"
            accessibleText: qsTrId("NextView")
            onClicked: root._restoreView(1)
        }

        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: 8
            Layout.preferredHeight: 1
        }

        BarIconButton {
            id: readerSinglePageButton
            objectName: "readerSinglePageButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady
                && root.allowedPageDisplayModes.indexOf(ReaderModes.SinglePageMode) >= 0
            checkable: true
            selected: root.pageDisplayMode
                === ReaderModes.SinglePageMode
            iconSource: "../assets/icons/page-single.svg"
            //% "Single Page"
            accessibleText: qsTrId("PageDisplaySinglePage")
            onClicked: root.pageDisplayModeRequested(
                ReaderModes.SinglePageMode)
        }

        BarIconButton {
            id: readerContinuousPageButton
            objectName: "readerContinuousPageButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady
                && root.allowedPageDisplayModes.indexOf(ReaderModes.ContinuousMode) >= 0
            checkable: true
            selected: root.pageDisplayMode
                === ReaderModes.ContinuousMode
            iconSource: "../assets/icons/page-continuous.svg"
            //% "Continuous"
            accessibleText: qsTrId("PageDisplayContinuous")
            onClicked: root.pageDisplayModeRequested(
                ReaderModes.ContinuousMode)
        }

        BarIconButton {
            id: readerFacingPageButton
            objectName: "readerFacingPageButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady
                && root.allowedPageDisplayModes.indexOf(ReaderModes.FacingMode) >= 0
            checkable: true
            selected: root.pageDisplayMode
                === ReaderModes.FacingMode
            iconSource: "../assets/icons/page-facing.svg"
            //% "Facing"
            accessibleText: qsTrId("PageDisplayFacing")
            onClicked: root.pageDisplayModeRequested(
                ReaderModes.FacingMode)
        }

        BarIconButton {
            id: readerContinuousFacingPageButton
            objectName: "readerContinuousFacingPageButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady
                && root.allowedPageDisplayModes.indexOf(ReaderModes.ContinuousFacingMode) >= 0
            checkable: true
            selected: root.pageDisplayMode
                === ReaderModes.ContinuousFacingMode
            iconSource: "../assets/icons/page-continuous-facing.svg"
            //% "Continuous Facing"
            accessibleText: qsTrId("PageDisplayContinuousFacing")
            onClicked: root.pageDisplayModeRequested(
                ReaderModes.ContinuousFacingMode)
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 20
            color: root.theme.ribbonBorder
            Accessible.ignored: true
        }

        BarIconButton {
            id: readerZoomOutButton
            objectName: "readerZoomOutButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady && root.zoom > 0.1001
            iconSource: "../assets/icons/zoom-out.svg"
            //% "Zoom Out"
            accessibleText: qsTrId("ZoomOut")
            onClicked: root._requestZoom(root._steppedZoom(-1))
        }

        Slider {
            id: readerZoomSlider
            objectName: "readerZoomSlider"

            Layout.preferredWidth: 132
            Layout.preferredHeight: root.controlSize
            enabled: root._interactionReady
            from: 0.0
            to: 1.0
            stepSize: 0.0
            focusPolicy: Qt.StrongFocus
            //% "Zoom"
            Accessible.name: qsTrId("Zoom")
            onMoved: {
                const targetZoom = root._sliderToZoom(value)
                if (pressed) {
                    root._sliderGestureChanged = Math.abs(
                        targetZoom - root._sliderStartZoom) >= 0.0001
                }
                root.zoomRequested(targetZoom)
            }
            onPressedChanged: {
                if (pressed) {
                    root._sliderStartPage = root.navigationPage
                    root._sliderStartZoom = root.zoom
                    root._sliderGestureChanged = false
                } else if (root._sliderGestureChanged) {
                    root._recordViewTransition(
                        root._sliderStartPage, root._sliderStartZoom,
                        root.navigationPage, root.zoom)
                    root._observedViewState = root._viewState(
                        root.navigationPage, root.zoom)
                    root._sliderGestureChanged = false
                }
            }

            Binding {
                target: readerZoomSlider
                property: "value"
                value: root._zoomToSlider(root.zoom)
                when: !readerZoomSlider.pressed
                restoreMode: Binding.RestoreBinding
            }

            background: Rectangle {
                x: readerZoomSlider.leftPadding
                y: readerZoomSlider.topPadding
                    + readerZoomSlider.availableHeight / 2 - height / 2
                implicitWidth: 132
                implicitHeight: 3
                width: readerZoomSlider.availableWidth
                height: 3
                radius: 2
                color: "#d7d3d0"

                Rectangle {
                    width: readerZoomSlider.visualPosition * parent.width
                    height: parent.height
                    radius: parent.radius
                    color: root.theme.foxitOrange
                }
            }

            handle: Rectangle {
                x: readerZoomSlider.leftPadding
                    + readerZoomSlider.visualPosition
                        * (readerZoomSlider.availableWidth - width)
                y: readerZoomSlider.topPadding
                    + readerZoomSlider.availableHeight / 2 - height / 2
                implicitWidth: 12
                implicitHeight: 12
                radius: 6
                color: readerZoomSlider.pressed
                    ? root.theme.foxitDarkOrange
                    : "#ffffff"
                border.width: 1
                border.color: readerZoomSlider.hovered
                        || readerZoomSlider.visualFocus
                    ? root.theme.foxitDarkOrange
                    : root.theme.foxitOrange
            }

            ToolTip.visible: hovered && !pressed
            ToolTip.delay: 500
            ToolTip.text: root._zoomText(root.zoom)
        }

        BarIconButton {
            id: readerZoomInButton
            objectName: "readerZoomInButton"

            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            theme: root.theme
            enabled: root._interactionReady && root.zoom < 4.9999
            iconSource: "../assets/icons/zoom-in.svg"
            //% "Zoom In"
            accessibleText: qsTrId("ZoomIn")
            onClicked: root._requestZoom(root._steppedZoom(1))
        }

        ComboBox {
            id: readerZoomComboBox
            objectName: "readerZoomComboBox"

            Layout.preferredWidth: 76
            Layout.preferredHeight: 26
            enabled: root._interactionReady
            model: root.zoomPresets
            displayText: root._zoomText(root.zoom)
            font.pixelSize: 12
            leftPadding: 8
            rightPadding: 22
            //% "Zoom"
            Accessible.name: qsTrId("Zoom")
            onActivated: index => {
                if (index >= 0 && index < root.zoomPresets.length)
                    root._requestZoom(root.zoomPresets[index] / 100.0)
            }

            Binding {
                target: readerZoomComboBox
                property: "currentIndex"
                value: root._zoomPresetIndex(root.zoom)
                restoreMode: Binding.RestoreBinding
            }

            delegate: ItemDelegate {
                required property int index
                required property var modelData

                width: readerZoomComboBox.popup
                    ? readerZoomComboBox.popup.width
                    : readerZoomComboBox.width
                text: root._zoomText(modelData / 100.0)
                font.pixelSize: 12
                highlighted: readerZoomComboBox.highlightedIndex === index
            }

            contentItem: Label {
                leftPadding: 2
                rightPadding: 2
                text: readerZoomComboBox.displayText
                color: readerZoomComboBox.enabled
                    ? root.theme.ribbonTextColor
                    : "#aaa8a6"
                font: readerZoomComboBox.font
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            background: Rectangle {
                radius: 3
                color: readerZoomComboBox.enabled ? "#ffffff" : "#f4f2f0"
                border.width: 1
                border.color: readerZoomComboBox.visualFocus
                    ? root.theme.foxitBorderOrange
                    : root.theme.ribbonBorder
            }

            ToolTip.visible: hovered
            ToolTip.delay: 500
            //% "Zoom"
            ToolTip.text: qsTrId("Zoom")
        }
    }
}
