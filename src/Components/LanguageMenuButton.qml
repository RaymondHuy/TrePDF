pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

RibbonButton {
    id: root
    objectName: "settingsLanguageButton"

    requiresDocument: false
    //% "Language"
    text: qsTrId("UiLanguageMenu")
    iconSource: Qt.resolvedUrl("../assets/icons/globe.svg")
    menuIndicatorVisible: true
    menuToolTipText: text
    primary: menuLoader.active
    onClicked: menuLoader.active = true
    onMenuRequested: menuLoader.active = true
    onEnabledChanged: {
        if (!enabled)
            menuLoader.active = false
    }
    onVisibleChanged: {
        if (!visible)
            menuLoader.active = false
    }

    function releaseClosedMenu() {
        if (menuLoader.status === Loader.Ready && menuLoader.item
                && !(menuLoader.item as Menu).visible)
            menuLoader.active = false
    }

    component LanguageItem: MenuItem {
        id: languageItem

        required property string language
        checkable: true
        autoExclusive: true
        checked: Qt.uiLanguage === language
        implicitHeight: 36
        font.pixelSize: 13
        palette.text: root.theme.ribbonTextColor
        palette.highlightedText: root.theme.ribbonTextColor
        onTriggered: Qt.uiLanguage = language

        background: Rectangle {
            radius: 4
            color: languageItem.highlighted
                ? root.theme.foxitPaleOrange : "transparent"
        }
    }

    Loader {
        id: menuLoader
        active: false
        onLoaded: {
            if (status === Loader.Ready && item)
                (item as Menu).open()
        }

        sourceComponent: Menu {
            id: languageMenu
            objectName: "settingsLanguageMenu"
            parent: root
            y: root.height
            width: 240
            height: Math.min(implicitHeight, root.Window.window
                ? Math.max(0, root.Window.window.height - 16) : implicitHeight)
            margins: 8
            padding: 4
            popupType: Popup.Item
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
            onOpened: {
                for (let index = 0; index < count; ++index) {
                    const option = itemAt(index) as MenuItem
                    if (option && option.checked) {
                        currentIndex = index
                        option.forceActiveFocus()
                        break
                    }
                }
            }
            onClosed: Qt.callLater(root.releaseClosedMenu)

            background: Rectangle {
                radius: 7
                color: root.theme.ribbonSurface
                border.width: 1
                border.color: root.theme.ribbonBorder
            }

            LanguageItem {
                objectName: "languageViItem"
                language: "vi"
                //% "Vietnamese"
                text: qsTrId("UiLanguageVietnamese")
            }
            LanguageItem {
                objectName: "languageEnItem"
                language: "en"
                //% "English"
                text: qsTrId("UiLanguageEnglish")
            }
            LanguageItem {
                objectName: "languageHrItem"
                language: "hr"
                //% "Croatian"
                text: qsTrId("UiLanguageCroatian")
            }
            LanguageItem {
                objectName: "languageThItem"
                language: "th"
                //% "Thai"
                text: qsTrId("UiLanguageThai")
            }
            LanguageItem {
                objectName: "languageFrItem"
                language: "fr"
                //% "French"
                text: qsTrId("UiLanguageFrench")
            }
            LanguageItem {
                objectName: "languageDeItem"
                language: "de"
                //% "German"
                text: qsTrId("UiLanguageGerman")
            }
            LanguageItem {
                objectName: "languageEsItem"
                language: "es"
                //% "Spanish"
                text: qsTrId("UiLanguageSpanish")
            }
            LanguageItem {
                objectName: "languagePtBrItem"
                language: "pt_BR"
                //% "Portuguese (Brazil)"
                text: qsTrId("UiLanguagePortugueseBrazil")
            }
            LanguageItem {
                objectName: "languageJaItem"
                language: "ja"
                //% "Japanese"
                text: qsTrId("UiLanguageJapanese")
            }
            LanguageItem {
                objectName: "languageZhCnItem"
                language: "zh_CN"
                //% "Chinese (Simplified)"
                text: qsTrId("UiLanguageChineseSimplified")
            }
            LanguageItem {
                objectName: "languageKoItem"
                language: "ko"
                //% "Korean"
                text: qsTrId("UiLanguageKorean")
            }
            LanguageItem {
                objectName: "languageIdItem"
                language: "id"
                //% "Indonesian"
                text: qsTrId("UiLanguageIndonesian")
            }
        }
    }
}
