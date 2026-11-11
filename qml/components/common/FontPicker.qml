pragma ComponentBehavior: Bound

import QtQuick
import SpanTui

AppComboBox {
    id: root

    property string selectedFamily: ""
    // Family names can legitimately be absent from the filtered list model —
    // the face still shows the stored family instead of going blank.
    displayText: currentIndex >= 0 && currentIndex < count
                 ? itemTextForIndex(currentIndex, model[currentIndex])
                 : selectedFamily
    signal familySelected(string family)

    function syncCurrentIndex() {
        const index = model.indexOf(selectedFamily)
        if (index >= 0 && currentIndex !== index)
            currentIndex = index
    }

    onSelectedFamilyChanged: syncCurrentIndex()
    onModelChanged: syncCurrentIndex()
    Component.onCompleted: syncCurrentIndex()
    onActivated: index => familySelected(model[index])

    delegate: ComboOptionRow {
        id: fontRow
        combo: root
        trailingWidth: 52 // style-ok: 'Aa' glyph sample cell

        RenderText {
            anchors.fill: parent
            text: qsTr("Aa")
            color: fontRow.onFill ? Theme.accentText : Theme.muted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            maximumLineCount: 1
            font.family: fontRow.modelData
            font.pixelSize: Theme.uiMd
        }
    }
}
