#pragma once

#include "application/application_constants.h"

#include <QFont>
#include <QObject>
#include <QPointer>
#include <QVariantList>

namespace span::application {

class SettingsStore;

// Language, theme, system display facts and the font lanes (QML singleton
// `Appearance`). The only preference object common/ and base/ QML may read.
// Without a store (gallery hosts) it serves the defaults.
//
// Font lanes: ui (primary text), uiAux (the "sm" tier: badges, metadata),
// formHint (SettingRow notes), editor and terminal. Each lane is a primary +
// fallback family picked from a list, a pixel size and a weight.
//
// Sized fonts are functions; QML cannot see what a function reads, so every
// binding passes `Appearance.fontRevision` as the last argument. The revision
// bumps on any typography change and makes those bindings re-evaluate.
class AppearanceSettings final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(bool effectiveDark READ effectiveDark NOTIFY appearanceChanged FINAL)
    // Windows "animation effects": when true, QML disables color and layout
    // transitions.
    Q_PROPERTY(bool reduceMotion READ reduceMotion NOTIFY reduceMotionChanged FINAL)
    Q_PROPERTY(int wheelScrollLines READ wheelScrollLines NOTIFY wheelScrollLinesChanged FINAL)
    Q_PROPERTY(int fontRevision READ fontRevision NOTIFY fontRevisionChanged FINAL)
    // Installed families, computed once: every face, and fixed-pitch only.
    Q_PROPERTY(QStringList availableFonts READ availableFonts CONSTANT FINAL)
    Q_PROPERTY(QStringList availableFixedPitchFonts READ availableFixedPitchFonts CONSTANT FINAL)
    Q_PROPERTY(QString uiPrimaryFontFamily READ uiPrimaryFontFamily WRITE setUiPrimaryFontFamily NOTIFY
                   appearanceChanged FINAL)
    Q_PROPERTY(QString uiFallbackFontFamily READ uiFallbackFontFamily WRITE setUiFallbackFontFamily NOTIFY
                   appearanceChanged FINAL)
    Q_PROPERTY(QVariantList availableUiFontWeights READ availableUiFontWeights NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(int uiFontSize READ uiFontSize WRITE setUiFontSize NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(int uiFontWeight READ uiFontWeight WRITE setUiFontWeight NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString uiAuxPrimaryFontFamily READ uiAuxPrimaryFontFamily WRITE setUiAuxPrimaryFontFamily NOTIFY
                   appearanceChanged FINAL)
    Q_PROPERTY(QString uiAuxFallbackFontFamily READ uiAuxFallbackFontFamily WRITE setUiAuxFallbackFontFamily NOTIFY
                   appearanceChanged FINAL)
    Q_PROPERTY(QVariantList availableUiAuxFontWeights READ availableUiAuxFontWeights NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(int uiAuxFontSize READ uiAuxFontSize WRITE setUiAuxFontSize NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(int uiAuxFontWeight READ uiAuxFontWeight WRITE setUiAuxFontWeight NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString formHintPrimaryFontFamily READ formHintPrimaryFontFamily WRITE setFormHintPrimaryFontFamily
                   NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString formHintFallbackFontFamily READ formHintFallbackFontFamily WRITE setFormHintFallbackFontFamily
                   NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(
        QVariantList availableFormHintFontWeights READ availableFormHintFontWeights NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(int formHintFontSize READ formHintFontSize WRITE setFormHintFontSize NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(
        int formHintFontWeight READ formHintFontWeight WRITE setFormHintFontWeight NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString editorPrimaryFontFamily READ editorPrimaryFontFamily WRITE setEditorPrimaryFontFamily NOTIFY
                   appearanceChanged FINAL)
    Q_PROPERTY(QString editorFallbackFontFamily READ editorFallbackFontFamily WRITE setEditorFallbackFontFamily NOTIFY
                   appearanceChanged FINAL)
    Q_PROPERTY(QVariantList availableEditorFontWeights READ availableEditorFontWeights NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(int editorFontSize READ editorFontSize WRITE setEditorFontSize NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(int editorFontWeight READ editorFontWeight WRITE setEditorFontWeight NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(double editorLineHeight READ editorLineHeight WRITE setEditorLineHeight NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString terminalPrimaryFontFamily READ terminalPrimaryFontFamily WRITE setTerminalPrimaryFontFamily
                   NOTIFY terminalFontChanged FINAL)
    Q_PROPERTY(QString terminalFallbackFontFamily READ terminalFallbackFontFamily WRITE setTerminalFallbackFontFamily
                   NOTIFY terminalFontChanged FINAL)
    Q_PROPERTY(
        QVariantList availableTerminalFontWeights READ availableTerminalFontWeights NOTIFY terminalFontChanged FINAL)
    Q_PROPERTY(int terminalFontSize READ terminalFontSize WRITE setTerminalFontSize NOTIFY terminalFontChanged FINAL)
    Q_PROPERTY(
        int terminalFontWeight READ terminalFontWeight WRITE setTerminalFontWeight NOTIFY terminalFontChanged FINAL)
    Q_PROPERTY(QFont terminalFont READ terminalFont NOTIFY terminalFontChanged FINAL)

public:
    explicit AppearanceSettings(SettingsStore *store = nullptr, QObject *parent = nullptr);

    // Reads every key from the store; call once after it opened.
    void load();

    QString language() const { return m_language; }
    // Used while nothing (or nothing valid) is stored: Simplified Chinese.
    static QString defaultLanguage() { return kLanguageChinese; }
    QString themeMode() const { return m_themeMode; }
    bool effectiveDark() const;
    bool reduceMotion() const { return m_reduceMotion; }
    int wheelScrollLines() const { return m_wheelScrollLines; }
    int fontRevision() const { return m_fontRevision; }

    QStringList availableFonts() const;
    QStringList availableFixedPitchFonts() const;
    QString uiPrimaryFontFamily() const { return m_fontLanes[UiLane].primary; }
    QString uiFallbackFontFamily() const { return m_fontLanes[UiLane].fallback; }
    QVariantList availableUiFontWeights() const { return availableFontWeights(UiLane); }
    int uiFontSize() const { return m_fontLanes[UiLane].size; }
    int uiFontWeight() const { return m_fontLanes[UiLane].weight; }
    QString uiAuxPrimaryFontFamily() const { return m_fontLanes[UiAuxLane].primary; }
    QString uiAuxFallbackFontFamily() const { return m_fontLanes[UiAuxLane].fallback; }
    QVariantList availableUiAuxFontWeights() const { return availableFontWeights(UiAuxLane); }
    int uiAuxFontSize() const { return m_fontLanes[UiAuxLane].size; }
    int uiAuxFontWeight() const { return m_fontLanes[UiAuxLane].weight; }
    QString formHintPrimaryFontFamily() const { return m_fontLanes[FormHintLane].primary; }
    QString formHintFallbackFontFamily() const { return m_fontLanes[FormHintLane].fallback; }
    QVariantList availableFormHintFontWeights() const { return availableFontWeights(FormHintLane); }
    int formHintFontSize() const { return m_fontLanes[FormHintLane].size; }
    int formHintFontWeight() const { return m_fontLanes[FormHintLane].weight; }
    QString editorPrimaryFontFamily() const { return m_fontLanes[EditorLane].primary; }
    QString editorFallbackFontFamily() const { return m_fontLanes[EditorLane].fallback; }
    QVariantList availableEditorFontWeights() const { return availableFontWeights(EditorLane); }
    int editorFontSize() const { return m_fontLanes[EditorLane].size; }
    int editorFontWeight() const { return m_fontLanes[EditorLane].weight; }
    double editorLineHeight() const { return m_editorLineHeight; }
    QString terminalPrimaryFontFamily() const { return m_fontLanes[TerminalLane].primary; }
    QString terminalFallbackFontFamily() const { return m_fontLanes[TerminalLane].fallback; }
    QVariantList availableTerminalFontWeights() const { return availableFontWeights(TerminalLane); }
    int terminalFontSize() const { return m_fontLanes[TerminalLane].size; }
    int terminalFontWeight() const { return m_fontLanes[TerminalLane].weight; }
    QFont terminalFont() const { return laneFont(TerminalLane, terminalFontSize()); }

    // Lane fonts at a pixel size, keeping the full fallback chain. The
    // revision argument only exists for QML bindings.
    Q_INVOKABLE QFont uiFont(int pixelSize, int revision = 0) const;
    Q_INVOKABLE QFont uiAuxFont(int pixelSize, int revision = 0) const;
    Q_INVOKABLE QFont formHintFont(int pixelSize, int revision = 0) const;
    Q_INVOKABLE QFont editorFont(int pixelSize, int revision = 0) const;
    Q_INVOKABLE bool isFixedPitch(const QString &family) const;

    void setLanguage(const QString &value);
    void setThemeMode(const QString &value);
    void setSystemDark(bool value);
    void setSystemWheelScrollLines(int value);
    // Re-reads the OS animation preference (cheap; call on focus / startup).
    void refreshSystemReduceMotion();
    void setUiPrimaryFontFamily(const QString &value) { setLanePrimary(UiLane, value); }
    void setUiFallbackFontFamily(const QString &value) { setLaneFallback(UiLane, value); }
    void setUiFontSize(int value) { setLaneSize(UiLane, value); }
    void setUiFontWeight(int value) { setLaneWeight(UiLane, value); }
    void setUiAuxPrimaryFontFamily(const QString &value) { setLanePrimary(UiAuxLane, value); }
    void setUiAuxFallbackFontFamily(const QString &value) { setLaneFallback(UiAuxLane, value); }
    void setUiAuxFontSize(int value) { setLaneSize(UiAuxLane, value); }
    void setUiAuxFontWeight(int value) { setLaneWeight(UiAuxLane, value); }
    void setFormHintPrimaryFontFamily(const QString &value) { setLanePrimary(FormHintLane, value); }
    void setFormHintFallbackFontFamily(const QString &value) { setLaneFallback(FormHintLane, value); }
    void setFormHintFontSize(int value) { setLaneSize(FormHintLane, value); }
    void setFormHintFontWeight(int value) { setLaneWeight(FormHintLane, value); }
    void setEditorPrimaryFontFamily(const QString &value) { setLanePrimary(EditorLane, value); }
    void setEditorFallbackFontFamily(const QString &value) { setLaneFallback(EditorLane, value); }
    void setEditorFontSize(int value) { setLaneSize(EditorLane, value); }
    void setEditorFontWeight(int value) { setLaneWeight(EditorLane, value); }
    void setEditorLineHeight(double value);
    void setTerminalPrimaryFontFamily(const QString &value) { setLanePrimary(TerminalLane, value); }
    void setTerminalFallbackFontFamily(const QString &value) { setLaneFallback(TerminalLane, value); }
    void setTerminalFontSize(int value) { setLaneSize(TerminalLane, value); }
    void setTerminalFontWeight(int value) { setLaneWeight(TerminalLane, value); }

signals:
    void appearanceChanged();
    // Only when effectiveDark() flips.
    void effectiveDarkChanged();
    void terminalFontChanged();
    void reduceMotionChanged();
    void wheelScrollLinesChanged();
    void fontRevisionChanged();

private:
    enum FontLaneId { UiLane, UiAuxLane, FormHintLane, EditorLane, TerminalLane, FontLaneCount };
    struct FontLane {
        QString primary;
        QString fallback;
        int size = 0;
        int weight = QFont::Normal;
    };
    // `key` prefixes the kSetting*Suffix keys in the settings table.
    struct FontLaneSpec {
        const char *key;
        const char *primary;
        const char *fallback;
        int defaultSize;
        int minSize;
        int maxSize;
    };
    static const FontLaneSpec &laneSpec(FontLaneId lane);
    // Nearest non-italic native face weight of `family`.
    static int nearestNativeWeight(const QString &family, int requested);
    // Snaps to the multipliers the settings UI offers.
    static double snappedLineHeight(double value);

    QStringList laneFamilyChoices(FontLaneId lane, bool primary) const;
    QStringList laneFamilyList(FontLaneId lane) const;
    QFont laneFont(FontLaneId lane, int pixelSize) const;
    QVariantList availableFontWeights(FontLaneId lane) const;
    void setLanePrimary(FontLaneId lane, const QString &value);
    void setLaneFallback(FontLaneId lane, const QString &value);
    void setLaneSize(FontLaneId lane, int value);
    void setLaneWeight(FontLaneId lane, int value);
    void storeLane(FontLaneId lane);
    void setEffectiveDarkInputs(const QString &themeMode, bool systemDark);
    void store(const QString &key, const QString &value);
    void ensureFontCaches() const;

    QPointer<SettingsStore> m_store;
    QString m_language = defaultLanguage();
    QString m_themeMode = kThemeSystem;
    bool m_systemDark = false;
    bool m_reduceMotion = false;
    int m_wheelScrollLines = 3;
    int m_fontRevision = 0;
    FontLane m_fontLanes[FontLaneCount];
    double m_editorLineHeight = 1.0;
    mutable bool m_fontCacheReady = false;
    mutable QStringList m_allFonts;
    mutable QStringList m_fixedPitchFonts;
};

} // namespace span::application
