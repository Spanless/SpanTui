import QtQuick
import SpanTui

AppComboBox {
    id: root

    property int selectedWeight: 400
    signal weightSelected(int weight)

    function optionText(option) {
        if (!option)
            return ""
        return String(option.style) + " · " + String(option.weight)
    }

    // Exact match wins; otherwise snap to the nearest listed weight —
    // the face never falls back to index 0 just because the stored
    // weight is not offered by the current family.
    function syncCurrentIndex() {
        var best = -1
        var bestDistance = Infinity
        for (let i = 0; i < count; ++i) {
            const w = Number(model[i].weight)
            if (w === selectedWeight) {
                best = i
                break
            }
            const distance = Math.abs(w - selectedWeight)
            if (distance < bestDistance) {
                bestDistance = distance
                best = i
            }
        }
        if (best >= 0 && currentIndex !== best)
            currentIndex = best
    }

    displayText: currentIndex >= 0 && currentIndex < count
                 ? optionText(model[currentIndex]) : ""
    itemTextForIndex: function(index, modelData) {
        return root.optionText(modelData)
    }
    onSelectedWeightChanged: syncCurrentIndex()
    onModelChanged: syncCurrentIndex()
    Component.onCompleted: syncCurrentIndex()
    // Echo the pick immediately — the app-state round trip re-syncs via
    // selectedWeight anyway, but the face must never look like the click
    // was ignored while that settles.
    onActivated: index => {
        if (index >= 0 && index < count)
            currentIndex = index
        weightSelected(Number(model[index].weight))
    }
}
