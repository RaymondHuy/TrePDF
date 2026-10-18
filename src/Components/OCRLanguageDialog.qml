pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF
import "ios"

Dialog {
    id: root
    objectName: "ocrLanguageDialog"

    required property AppTheme theme
    property var currentCodes: ["eng", "vie"]
    property var selectedCodes: []
    property bool initialFocusPending: false
    readonly property int maximumLanguages: 4
    readonly property var selectedLanguages: languageManager.languages.filter(
        language => root.selectedCodes.indexOf(language.code) !== -1)
    readonly property var missingLanguages: selectedLanguages.filter(
        language => !language.installed)
    readonly property double downloadBytes: missingLanguages.reduce(
        (total, language) => total + Number(language.bytes), 0)
    readonly property var filteredLanguages: languageManager.languages.filter(language => {
        const query = languageSearch.text.trim().toLocaleLowerCase()
        return query.length === 0 || (language.name + " " + language.nativeName + " "
            + language.code).toLocaleLowerCase().indexOf(query) !== -1
    })
    readonly property bool canSubmit: !languageManager.busy
        && selectedCodes.length > 0 && selectedCodes.length <= maximumLanguages

    signal languagesSelected(var codes, string label)

    parent: Overlay.overlay
    anchors.centerIn: dialogMetrics.active ? undefined : parent
    x: dialogMetrics.active
        ? dialogMetrics.leftInset + (dialogMetrics.availableWidth - width) / 2 : 0
    y: dialogMetrics.active
        ? dialogMetrics.topInset + Math.max(8, (dialogMetrics.availableHeight - height) / 2) : 0
    width: Math.min(600, parent ? dialogMetrics.availableWidth - 32 : 600)
    height: Math.min(690, parent ? dialogMetrics.availableHeight - 32 : 690)
    padding: 20
    modal: true
    focus: true
    popupType: Popup.Item
    closePolicy: Popup.CloseOnEscape
    //% "Which languages are in your PDF?"
    title: qsTrId("OCRChooseLanguagesTitle")

    DialogMetrics {
        id: dialogMetrics
        overlay: root.parent
    }

    OCRLanguageManager {
        id: languageManager
        objectName: "ocrLanguageManager"

        onChanged: {
            if (root.visible && root.initialFocusPending && !busy) {
                root.initialFocusPending = false
                languageSearch.forceActiveFocus()
            }
        }

        onReady: codes => {
            if (!root.visible)
                return
            const names = codes.map(code => {
                const language = languageManager.languages.find(item => item.code === code)
                return language ? language.name : code
            })
            root.languagesSelected(codes, names.join(", "))
            root.accept()
        }
    }

    function setLanguageSelected(code: string, selected: bool) {
        if (languageManager.busy)
            return
        const next = selectedCodes.filter(item => item !== code)
        if (selected && next.length < maximumLanguages)
            next.push(code)
        selectedCodes = next
    }

    function submit() {
        if (canSubmit)
            languageManager.prepare(selectedCodes)
    }

    onOpened: {
        selectedCodes = currentCodes.slice()
        initialFocusPending = true
        languageManager.refresh()
        if (!languageManager.busy) {
            initialFocusPending = false
            languageSearch.forceActiveFocus()
        }
    }
    onAboutToHide: languageManager.cancel()

    background: Rectangle {
        color: root.theme.ribbonSurface
        border.color: root.theme.ribbonBorder
        radius: 6
    }

    header: Label {
        text: root.title
        padding: 20
        bottomPadding: 0
        font.pixelSize: 18
        font.weight: Font.DemiBold
        color: root.theme.ribbonTextColor
        wrapMode: Text.Wrap
    }

    contentItem: ScrollView {
        id: dialogScroll
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        contentData: [languageFields]
    }

    readonly property ColumnLayout _languageFields: ColumnLayout {
        id: languageFields
        width: dialogScroll.availableWidth
        height: Math.max(implicitHeight, dialogScroll.availableHeight)
        spacing: 12

        Label {
            Layout.fillWidth: true
            //% "Choose the languages that appear in the document so OCR can recognize letters and accents correctly. You can select more than one for a multilingual PDF."
            text: qsTrId("OCRChooseLanguagesExplanation")
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            //% "English and Vietnamese are included. Other languages download once, then work offline. Your PDF stays on your computer."
            text: qsTrId("OCRLanguageDownloadExplanation")
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }
        TextField {
            id: languageSearch
            objectName: "ocrLanguageSearch"
            Layout.fillWidth: true
            enabled: !languageManager.busy
            maximumLength: 100
            selectByMouse: true
            //% "Search languages"
            placeholderText: qsTrId("OCRSearchLanguages")
            Accessible.name: placeholderText
            Keys.onDownPressed: {
                if (languageList.count > 0) {
                    languageList.currentIndex = 0
                    languageList.forceActiveFocus()
                    if (languageList.currentItem)
                        languageList.currentItem.forceActiveFocus()
                }
            }
        }
        Label {
            objectName: "ocrLanguageSelectionHint"
            Layout.fillWidth: true
            //% "%1 of %2 selected. Choose only languages present in the PDF for better accuracy."
            text: qsTrId("OCRLanguageSelectionHint")
                .arg(root.selectedCodes.length).arg(root.maximumLanguages)
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }
        ProgressBar {
            objectName: "ocrLanguageDownloadProgress"
            Layout.fillWidth: true
            visible: languageManager.busy
            from: 0
            to: 1
            value: languageManager.progress
            //% "Language download progress"
            Accessible.name: qsTrId("OCRLanguageDownloadProgress")
        }
        Label {
            objectName: "ocrLanguageDownloadStatus"
            Layout.fillWidth: true
            visible: languageManager.busy && text.length > 0
            text: languageManager.statusText
            textFormat: Text.PlainText
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "ocrLanguageDownloadError"
            Layout.fillWidth: true
            visible: languageManager.errorText.length > 0
            text: languageManager.errorText
            textFormat: Text.PlainText
            color: "#b3261e"
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 250
            Layout.preferredHeight: 250
            radius: 4
            color: root.theme.ribbonSurface
            border.color: root.theme.ribbonBorder

            ListView {
                id: languageList
                objectName: "ocrLanguageList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: root.filteredLanguages
                reuseItems: true
                keyNavigationEnabled: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: CheckDelegate {
                    id: languageChoice
                    required property var modelData
                    objectName: "ocrLanguageChoice_" + modelData.code
                    width: languageList.width
                    implicitHeight: Math.max(58, implicitContentHeight + topPadding + bottomPadding)
                    checked: root.selectedCodes.indexOf(modelData.code) !== -1
                    enabled: !languageManager.busy
                        && (checked || root.selectedCodes.length < root.maximumLanguages)
                    text: modelData.name
                    onToggled: root.setLanguageSelected(modelData.code, checked)
                    Accessible.description: modelData.installed
                        //% "Ready to use offline"
                        ? qsTrId("OCRLanguageReadyOffline")
                        //% "Downloads when you apply your selection"
                        : qsTrId("OCRLanguageDownloadsOnApply")

                    contentItem: ColumnLayout {
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: languageChoice.modelData.name
                            font.weight: Font.DemiBold
                            color: root.theme.ribbonTextColor
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: languageChoice.modelData.nativeName
                            visible: text !== languageChoice.modelData.name
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: languageChoice.modelData.installed
                                //% "Ready to use offline"
                                ? qsTrId("OCRLanguageReadyOffline")
                                //% "Download · %1 MB"
                                : qsTrId("OCRLanguageDownloadSize").arg(
                                    (Number(languageChoice.modelData.bytes) / 1048576).toFixed(1))
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: languageList.count === 0
                    //% "No matching languages. Try another name."
                    text: qsTrId("OCRNoMatchingLanguages")
                    color: root.theme.ribbonTextColor
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }
            }
        }
        Label {
            objectName: "ocrLanguageDownloadSize"
            Layout.fillWidth: true
            visible: !languageManager.busy && root.missingLanguages.length > 0
            //% "Download once: %1 MB. An internet connection is needed for this step."
            text: qsTrId("OCRLanguageTotalDownloadSize")
                .arg((root.downloadBytes / 1048576).toFixed(1))
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }

    }

    footer: DialogButtonBox {
        padding: 16
        spacing: 8
        alignment: Qt.AlignRight

        Button {
            objectName: "cancelOCRLanguagesButton"
            //% "Cancel"
            text: qsTrId("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
        OrangeButton {
            objectName: "applyOCRLanguagesButton"
            theme: root.theme
            enabled: root.canSubmit
            text: languageManager.errorText.length > 0
                //% "Try again"
                ? qsTrId("OCRLanguageRetry")
                : root.missingLanguages.length > 0
                    //% "Download and use"
                    ? qsTrId("OCRLanguageDownloadAndUse")
                    //% "Use these languages"
                    : qsTrId("OCRUseSelectedLanguages")
            onClicked: root.submit()
        }
    }
}
