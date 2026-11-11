#include "terminal/render/terminal_input_routing.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QtTest>

#include <memory>

using namespace span::terminal;

namespace {

// Bits Qt's Windows platform plugin reports in QKeyEvent::nativeModifiers().
constexpr quint32 kNativeShiftLeft = 0x00000001;
constexpr quint32 kNativeControlLeft = 0x00000002;
constexpr quint32 kNativeControlRight = 0x00000020;
constexpr quint32 kNativeAltRight = 0x00000040;
constexpr quint32 kNativeNumLock = 0x00000200;
constexpr quint32 kNativeExtendedKey = 0x01000000;

struct KeySpec {
    int key = 0;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    QString text;
    quint32 virtualKey = 0;
    quint32 scanCode = 0;
    quint32 nativeModifiers = 0;
    QEvent::Type type = QEvent::KeyPress;
};

std::unique_ptr<QKeyEvent> keyEvent(const KeySpec &spec)
{
    return std::make_unique<QKeyEvent>(spec.type, spec.key, spec.modifiers, spec.scanCode,
                                       spec.virtualKey, spec.nativeModifiers, spec.text);
}

std::optional<TerminalKeyStroke> translate(const KeySpec &spec, bool altGr = false,
                                           TerminalKeyEvent event = TerminalKeyEvent::Press)
{
    const auto qtEvent = keyEvent(spec);
    return TerminalInputRouting::translateKey(qtEvent.get(), altGr, event);
}

QString strokeSummary(int key, TerminalKeyEvent event, int modifiers, quint16 virtualKey, quint16 scanCode,
                      quint32 controlKeyState)
{
    return QStringLiteral("key %1 event %2 modifiers %3 vk %4 scan %5 state %6")
        .arg(key)
        .arg(int(event))
        .arg(modifiers)
        .arg(virtualKey)
        .arg(scanCode)
        .arg(controlKeyState);
}

QStringList strokeSummaries(const QVector<TerminalKeyStroke> &strokes)
{
    QStringList summaries;
    for (const TerminalKeyStroke &stroke : strokes) {
        summaries.append(strokeSummary(stroke.key ? int(*stroke.key) : -1, stroke.event, stroke.modifiers.toInt(),
                                       stroke.native.virtualKey, stroke.native.scanCode,
                                       stroke.native.controlKeyState));
    }
    return summaries;
}

} // namespace

class TerminalInputRoutingTest final : public QObject {
    Q_OBJECT
private slots:
    void translatesFunctionalKeys_data();
    void translatesFunctionalKeys();
    void translatesTextKeys_data();
    void translatesTextKeys();
    void rejectsKeysThatAreNotTerminalInput();
    void releaseKeepsPressTranslation();
    void buildsNativeKeyRecord_data();
    void buildsNativeKeyRecord();
    void physicalKeyIsStableAcrossShift();
    void identifiesAltGr();
    void shortcutTableMapsSingleChords();
    void plainCtrlCIgnoresOtherChords();
    void mouseStaysLocalUnlessProgramTracksIt();
    void mapsMouseButtons();
    void marksKeypadKeys();
    void scrollsAlternateScreenWithCursorKeys();
};

void TerminalInputRoutingTest::translatesFunctionalKeys_data()
{
    QTest::addColumn<int>("qtKey");
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
    QTest::addColumn<int>("expectedKey");
    QTest::addColumn<int>("expectedFunction");
    QTest::addColumn<Qt::KeyboardModifiers>("expectedModifiers");
    const Qt::KeyboardModifiers none = Qt::NoModifier;
    const auto row = [](const char *name, int qtKey, Qt::KeyboardModifiers modifiers,
                        TerminalKey key, int function, Qt::KeyboardModifiers expected) {
        QTest::newRow(name) << qtKey << modifiers << int(key) << function << expected;
    };
    row("return", Qt::Key_Return, none, TerminalKey::Enter, 0, none);
    row("keypad-enter", Qt::Key_Enter, Qt::KeypadModifier, TerminalKey::Enter, 0, none);
    row("tab", Qt::Key_Tab, none, TerminalKey::Tab, 0, none);
    // Qt may deliver Shift+Tab as Backtab without the Shift flag.
    row("backtab-adds-shift", Qt::Key_Backtab, none, TerminalKey::Tab, 0, Qt::ShiftModifier);
    row("backspace", Qt::Key_Backspace, none, TerminalKey::Backspace, 0, none);
    row("escape", Qt::Key_Escape, none, TerminalKey::Escape, 0, none);
    row("ctrl-up", Qt::Key_Up, Qt::ControlModifier, TerminalKey::Up, 0, Qt::ControlModifier);
    row("down", Qt::Key_Down, none, TerminalKey::Down, 0, none);
    row("alt-left", Qt::Key_Left, Qt::AltModifier, TerminalKey::Left, 0, Qt::AltModifier);
    row("keypad-right", Qt::Key_Right, Qt::KeypadModifier, TerminalKey::Right, 0, none);
    row("insert", Qt::Key_Insert, none, TerminalKey::Insert, 0, none);
    row("delete", Qt::Key_Delete, none, TerminalKey::Delete, 0, none);
    row("home", Qt::Key_Home, none, TerminalKey::Home, 0, none);
    row("end", Qt::Key_End, none, TerminalKey::End, 0, none);
    row("shift-page-up", Qt::Key_PageUp, Qt::ShiftModifier, TerminalKey::PageUp, 0,
        Qt::ShiftModifier);
    row("page-down", Qt::Key_PageDown, none, TerminalKey::PageDown, 0, none);
    row("f1", Qt::Key_F1, none, TerminalKey::Function, 1, none);
    row("meta-f12", Qt::Key_F12, Qt::MetaModifier, TerminalKey::Function, 12, Qt::MetaModifier);
    row("f35", Qt::Key_F35, none, TerminalKey::Function, 35, none);
}

void TerminalInputRoutingTest::translatesFunctionalKeys()
{
    QFETCH(int, qtKey);
    QFETCH(Qt::KeyboardModifiers, modifiers);
    QFETCH(int, expectedKey);
    QFETCH(int, expectedFunction);
    QFETCH(Qt::KeyboardModifiers, expectedModifiers);
    const auto stroke = translate({qtKey, modifiers});
    QVERIFY(stroke.has_value());
    QVERIFY(stroke->key.has_value());
    QCOMPARE(int(*stroke->key), expectedKey);
    QCOMPARE(stroke->functionNumber, expectedFunction);
    QCOMPARE(stroke->modifiers.toInt(), expectedModifiers.toInt());
    QVERIFY(stroke->text.isEmpty());
}

void TerminalInputRoutingTest::translatesTextKeys_data()
{
    QTest::addColumn<int>("qtKey");
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("virtualKey");
    QTest::addColumn<bool>("altGr");
    QTest::addColumn<QString>("expectedText");
    QTest::addColumn<Qt::KeyboardModifiers>("expectedModifiers");
    QTest::addColumn<int>("codepoint");
    QTest::addColumn<int>("shiftedCodepoint");
    QTest::addColumn<int>("baseLayoutCodepoint");
    const Qt::KeyboardModifiers none = Qt::NoModifier;
    const Qt::KeyboardModifiers shift = Qt::ShiftModifier;
    const Qt::KeyboardModifiers ctrl = Qt::ControlModifier;
    const Qt::KeyboardModifiers alt = Qt::AltModifier;

    QTest::newRow("plain-letter")
        << int(Qt::Key_A) << none << QStringLiteral("a") << int('A') << false
        << QStringLiteral("a") << none << int('a') << 0 << int('a');
    QTest::newRow("shift-letter")
        << int(Qt::Key_A) << shift << QStringLiteral("A") << int('A') << false
        << QStringLiteral("A") << shift << int('a') << int('A') << int('a');
    // Shift+2 on a US layout: identity is the unshifted key, the produced
    // character is the shifted alternate.
    QTest::newRow("shift-digit-symbol")
        << int(Qt::Key_At) << shift << QStringLiteral("@") << int('2') << false
        << QStringLiteral("@") << shift << int('2') << int('@') << int('2');
    QTest::newRow("shift-punctuation")
        << int(Qt::Key_QuoteDbl) << shift << QStringLiteral("\"") << 0xDE << false
        << QStringLiteral("\"") << shift << int('\'') << int('"') << 0;
    // Windows hands Ctrl chords over as control characters; the key's own
    // character is recovered for the encoder.
    QTest::newRow("ctrl-letter-control-text")
        << int(Qt::Key_C) << ctrl << QStringLiteral("\x03") << int('C') << false
        << QStringLiteral("c") << ctrl << int('c') << 0 << int('c');
    QTest::newRow("ctrl-bracket-control-text")
        << int(Qt::Key_BracketLeft) << ctrl << QStringLiteral("\x1b") << 0xDB << false
        << QStringLiteral("[") << ctrl << int('[') << 0 << 0;
    QTest::newRow("ctrl-digit-no-text")
        << int(Qt::Key_2) << ctrl << QString() << int('2') << false
        << QStringLiteral("2") << ctrl << int('2') << 0 << int('2');
    QTest::newRow("alt-letter-no-text")
        << int(Qt::Key_X) << alt << QString() << int('X') << false
        << QStringLiteral("x") << alt << int('x') << 0 << int('x');
    // AltGr arrives as Ctrl+Alt; its character is the meaning, not a chord.
    QTest::newRow("altgr-character")
        << int(Qt::Key_At) << (ctrl | alt) << QStringLiteral("@") << int('Q') << true
        << QStringLiteral("@") << none << int('@') << 0 << int('q');
    QTest::newRow("ctrl-alt-without-altgr")
        << int(Qt::Key_Q) << (ctrl | alt) << QStringLiteral("@") << int('Q') << false
        << QStringLiteral("q") << (ctrl | alt) << int('q') << 0 << int('q');
    // A non-Latin layout keeps its character and reports the base-layout key
    // so shortcuts still resolve.
    QTest::newRow("cyrillic-layout")
        << 0x0421 << none << QString(QChar(0x0441)) << int('C') << false
        << QString(QChar(0x0441)) << none << 0x0441 << 0 << int('c');
    // IME commits delivered as a key event carry no key identity.
    QTest::newRow("ime-multi-codepoint")
        << int(Qt::Key_unknown) << none << QStringLiteral("你好") << 0xE5 << false
        << QStringLiteral("你好") << none << 0 << 0 << 0;
}

void TerminalInputRoutingTest::translatesTextKeys()
{
    QFETCH(int, qtKey);
    QFETCH(Qt::KeyboardModifiers, modifiers);
    QFETCH(QString, text);
    QFETCH(int, virtualKey);
    QFETCH(bool, altGr);
    QFETCH(QString, expectedText);
    QFETCH(Qt::KeyboardModifiers, expectedModifiers);
    QFETCH(int, codepoint);
    QFETCH(int, shiftedCodepoint);
    QFETCH(int, baseLayoutCodepoint);

    KeySpec spec{qtKey, modifiers, text, quint32(virtualKey)};
    const auto stroke = translate(spec, altGr);
    QVERIFY(stroke.has_value());
    QVERIFY(!stroke->key.has_value());
    QCOMPARE(stroke->text, expectedText);
    QCOMPARE(stroke->modifiers.toInt(), expectedModifiers.toInt());
    QCOMPARE(stroke->textKey.codepoint, codepoint);
    QCOMPARE(stroke->textKey.shiftedCodepoint, shiftedCodepoint);
    QCOMPARE(stroke->textKey.baseLayoutCodepoint, baseLayoutCodepoint);
}

void TerminalInputRoutingTest::rejectsKeysThatAreNotTerminalInput()
{
    QVERIFY(!TerminalInputRouting::translateKey(nullptr, false, TerminalKeyEvent::Press));
    // Modifier-only press.
    QVERIFY(!translate({Qt::Key_Shift, Qt::ShiftModifier}));
    QVERIFY(!translate({Qt::Key_Control, Qt::ControlModifier}));
    // Win-key chords belong to the desktop.
    QVERIFY(!translate({Qt::Key_E, Qt::MetaModifier, QStringLiteral("e"), 'E'}));
    // A Ctrl chord on a key with neither text nor an ASCII identity.
    QVERIFY(!translate({Qt::Key_Dead_Acute, Qt::ControlModifier}));
}

void TerminalInputRoutingTest::releaseKeepsPressTranslation()
{
    const KeySpec press{Qt::Key_P, Qt::ControlModifier, QStringLiteral("\x10"), 'P', 0x19};
    KeySpec release = press;
    release.type = QEvent::KeyRelease;
    const auto pressed = translate(press, false, TerminalKeyEvent::Press);
    const auto released = translate(release, false, TerminalKeyEvent::Release);
    QVERIFY(pressed && released);
    QCOMPARE(int(pressed->event), int(TerminalKeyEvent::Press));
    QCOMPARE(int(released->event), int(TerminalKeyEvent::Release));
    QCOMPARE(released->text, pressed->text);
    QCOMPARE(released->modifiers.toInt(), pressed->modifiers.toInt());
    QCOMPARE(released->textKey.codepoint, pressed->textKey.codepoint);
    QCOMPARE(released->native.virtualKey, pressed->native.virtualKey);

    const auto repeated = translate({Qt::Key_Down}, false, TerminalKeyEvent::Repeat);
    QVERIFY(repeated);
    QCOMPARE(int(repeated->event), int(TerminalKeyEvent::Repeat));
}

void TerminalInputRoutingTest::buildsNativeKeyRecord_data()
{
    namespace State = TerminalControlKeyState;
    QTest::addColumn<int>("qtKey");
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("virtualKey");
    QTest::addColumn<int>("scanCode");
    QTest::addColumn<int>("nativeModifiers");
    QTest::addColumn<int>("expectedVirtualKey");
    QTest::addColumn<int>("expectedScanCode");
    QTest::addColumn<int>("expectedState");
    QTest::addColumn<QString>("expectedText");

    QTest::newRow("shift-letter")
        << int(Qt::Key_A) << Qt::KeyboardModifiers(Qt::ShiftModifier) << QStringLiteral("A")
        << 0x41 << 0x1E << int(kNativeShiftLeft | kNativeNumLock)
        << 0x41 << 0x1E << int(State::Shift | State::NumLock) << QStringLiteral("A");
    // The extended-key bit leaves the scan code and becomes ENHANCED_KEY.
    QTest::newRow("extended-arrow")
        << int(Qt::Key_Right) << Qt::KeyboardModifiers(Qt::ControlModifier) << QString()
        << 0x27 << 0x14D << int(kNativeControlRight | kNativeExtendedKey)
        << 0x27 << 0x4D << int(State::RightCtrl | State::EnhancedKey) << QString();
    // AltGr reaches the console program as left Ctrl + right Alt.
    QTest::newRow("altgr")
        << int(Qt::Key_At) << Qt::KeyboardModifiers(Qt::ControlModifier | Qt::AltModifier)
        << QStringLiteral("@") << 0x51 << 0x10 << int(kNativeControlLeft | kNativeAltRight)
        << 0x51 << 0x10 << int(State::LeftCtrl | State::RightAlt) << QStringLiteral("@");
    // Synthesized events have no native state: derive it from Qt modifiers.
    QTest::newRow("synthesized-modifiers")
        << int(Qt::Key_A)
        << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier)
        << QString() << 0x41 << 0 << 0
        << 0x41 << 0 << int(State::Shift | State::LeftCtrl | State::LeftAlt) << QString();
    // No physical identity: IME process key, VK_PACKET, none, out of range.
    QTest::newRow("ime-process-key")
        << int(Qt::Key_unknown) << Qt::KeyboardModifiers() << QStringLiteral("字")
        << 0xE5 << 0x1E << 0 << 0 << 0 << 0 << QString();
    QTest::newRow("unicode-packet")
        << int(Qt::Key_unknown) << Qt::KeyboardModifiers() << QStringLiteral("é")
        << 0xE7 << 0 << 0 << 0 << 0 << 0 << QString();
    QTest::newRow("no-virtual-key")
        << int(Qt::Key_A) << Qt::KeyboardModifiers() << QStringLiteral("a")
        << 0 << 0 << 0 << 0 << 0 << 0 << QString();
    QTest::newRow("virtual-key-out-of-range")
        << int(Qt::Key_A) << Qt::KeyboardModifiers() << QStringLiteral("a")
        << 0x1FF << 0x1E << 0 << 0 << 0 << 0 << QString();
}

void TerminalInputRoutingTest::buildsNativeKeyRecord()
{
    QFETCH(int, qtKey);
    QFETCH(Qt::KeyboardModifiers, modifiers);
    QFETCH(QString, text);
    QFETCH(int, virtualKey);
    QFETCH(int, scanCode);
    QFETCH(int, nativeModifiers);
    QFETCH(int, expectedVirtualKey);
    QFETCH(int, expectedScanCode);
    QFETCH(int, expectedState);
    QFETCH(QString, expectedText);

    const auto stroke = translate({qtKey, modifiers, text, quint32(virtualKey), quint32(scanCode),
                                   quint32(nativeModifiers)});
    QVERIFY(stroke.has_value());
    QCOMPARE(int(stroke->native.virtualKey), expectedVirtualKey);
    QCOMPARE(int(stroke->native.scanCode), expectedScanCode);
    QCOMPARE(int(stroke->native.controlKeyState), expectedState);
    QCOMPARE(stroke->native.text, expectedText);
}

void TerminalInputRoutingTest::physicalKeyIsStableAcrossShift()
{
    QCOMPARE(TerminalInputRouting::physicalKey(nullptr), 0u);
    // Shift+2 presses '@'; Shift released first, the release names '2'.
    const auto press = keyEvent({Qt::Key_At, Qt::ShiftModifier, QStringLiteral("@"), '2', 0x03});
    const auto release = keyEvent({Qt::Key_2, Qt::NoModifier, QStringLiteral("2"), '2', 0x03,
                                   0, QEvent::KeyRelease});
    QCOMPARE(TerminalInputRouting::physicalKey(press.get()), 0x03u);
    QCOMPARE(TerminalInputRouting::physicalKey(release.get()),
             TerminalInputRouting::physicalKey(press.get()));
    // Synthesized events fall back to the Qt key.
    const auto synthetic = keyEvent({Qt::Key_A});
    QCOMPARE(TerminalInputRouting::physicalKey(synthetic.get()), quint32(Qt::Key_A));
}

void TerminalInputRoutingTest::identifiesAltGr()
{
    QVERIFY(!TerminalInputRouting::identifiesAltGr(nullptr));
    QVERIFY(TerminalInputRouting::identifiesAltGr(keyEvent({Qt::Key_AltGr}).get()));
    // VK_RMENU
    QVERIFY(TerminalInputRouting::identifiesAltGr(
        keyEvent({Qt::Key_Alt, Qt::AltModifier, QString(), 0xA5}).get()));
    QVERIFY(TerminalInputRouting::identifiesAltGr(
        keyEvent({Qt::Key_Q, Qt::GroupSwitchModifier, QStringLiteral("@")}).get()));
    // Left Alt (VK_LMENU) plus Ctrl is an ordinary chord.
    QVERIFY(!TerminalInputRouting::identifiesAltGr(
        keyEvent({Qt::Key_Alt, Qt::ControlModifier | Qt::AltModifier, QString(), 0xA4}).get()));
}

void TerminalInputRoutingTest::shortcutTableMapsSingleChords()
{
    const QVariantMap bindings{
        {QStringLiteral("copy"), QStringLiteral("Ctrl+Shift+C")},
        {QStringLiteral("copyAlias"), QStringLiteral("Ctrl+Shift+C")},
        {QStringLiteral("paste"), QStringLiteral("Ctrl+Shift+V")},
        {QStringLiteral("multiChord"), QStringLiteral("Ctrl+K, Ctrl+S")},
        {QStringLiteral("unbound"), QString()},
    };
    const QHash<int, QStringList> table = TerminalInputRouting::shortcutTable(bindings);
    QCOMPARE(table.size(), 2);

    const auto copy = keyEvent({Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier});
    QCOMPARE(TerminalInputRouting::shortcutActions(copy.get(), table),
             (QStringList{QStringLiteral("copy"), QStringLiteral("copyAlias")}));
    // Keypad and group-switch flags are not part of a binding.
    const auto keypadPaste = keyEvent(
        {Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier | Qt::KeypadModifier});
    QCOMPARE(TerminalInputRouting::shortcutActions(keypadPaste.get(), table),
             QStringList{QStringLiteral("paste")});

    QVERIFY(TerminalInputRouting::shortcutActions(
                keyEvent({Qt::Key_C, Qt::ControlModifier}).get(), table).isEmpty());
    QVERIFY(TerminalInputRouting::shortcutActions(
                keyEvent({Qt::Key_K, Qt::ControlModifier}).get(), table).isEmpty());
    QVERIFY(TerminalInputRouting::shortcutActions(nullptr, table).isEmpty());
    QVERIFY(TerminalInputRouting::shortcutActions(copy.get(), {}).isEmpty());
}

void TerminalInputRoutingTest::plainCtrlCIgnoresOtherChords()
{
    const auto ctrlC = keyEvent({Qt::Key_C, Qt::ControlModifier});
    QVERIFY(TerminalInputRouting::isPlainCtrlC(ctrlC.get(), Qt::ControlModifier));
    QVERIFY(!TerminalInputRouting::isPlainCtrlC(ctrlC.get(),
                                                Qt::ControlModifier | Qt::ShiftModifier));
    QVERIFY(!TerminalInputRouting::isPlainCtrlC(
        keyEvent({Qt::Key_V, Qt::ControlModifier}).get(), Qt::ControlModifier));
    QVERIFY(!TerminalInputRouting::isPlainCtrlC(nullptr, Qt::ControlModifier));
}

void TerminalInputRoutingTest::mouseStaysLocalUnlessProgramTracksIt()
{
    const QMouseEvent plain(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    const QMouseEvent shifted(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1),
                              Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    // No mouse tracking: selection and links are always local.
    QVERIFY(TerminalInputRouting::useLocalMouse(0, &plain));
    QVERIFY(TerminalInputRouting::useLocalMouse(0, nullptr));
    // Tracking: the program gets the mouse; Shift takes it back for selection.
    QVERIFY(!TerminalInputRouting::useLocalMouse(1, &plain));
    QVERIFY(TerminalInputRouting::useLocalMouse(1, &shifted));
    QVERIFY(!TerminalInputRouting::useLocalMouse(1, nullptr));
}

void TerminalInputRoutingTest::mapsMouseButtons()
{
    QCOMPARE(int(TerminalInputRouting::mouseButton(Qt::LeftButton)), int(TerminalMouseButton::Left));
    QCOMPARE(int(TerminalInputRouting::mouseButton(Qt::MiddleButton)), int(TerminalMouseButton::Middle));
    QCOMPARE(int(TerminalInputRouting::mouseButton(Qt::RightButton)), int(TerminalMouseButton::Right));
    QCOMPARE(int(TerminalInputRouting::mouseButton(Qt::BackButton)), int(TerminalMouseButton::Left));
}

void TerminalInputRoutingTest::marksKeypadKeys()
{
    QVERIFY(translate({Qt::Key_1, Qt::KeypadModifier, QStringLiteral("1")})->keypad);
    QVERIFY(!translate({Qt::Key_1, Qt::NoModifier, QStringLiteral("1")})->keypad);
    QVERIFY(translate({Qt::Key_Enter, Qt::KeypadModifier})->keypad);
    QVERIFY(!translate({Qt::Key_Return})->keypad);
    // NumLock-off keypad navigation stays the main-cluster key.
    QVERIFY(!translate({Qt::Key_Right, Qt::KeypadModifier})->keypad);
}

void TerminalInputRoutingTest::scrollsAlternateScreenWithCursorKeys()
{
    const QVector<TerminalKeyStroke> up = TerminalInputRouting::alternateScrollKeys(2);
    // A press and release of the bare arrow per line; a console program
    // reading records sees the real arrow key.
    const QString press = strokeSummary(int(TerminalKey::Up), TerminalKeyEvent::Press, 0, 0x26, 0x48,
                                        TerminalControlKeyState::EnhancedKey);
    const QString release = strokeSummary(int(TerminalKey::Up), TerminalKeyEvent::Release, 0, 0x26, 0x48,
                                          TerminalControlKeyState::EnhancedKey);
    QCOMPARE(strokeSummaries(up), (QStringList{press, release, press, release}));
    const QVector<TerminalKeyStroke> down = TerminalInputRouting::alternateScrollKeys(-1);
    QCOMPARE(down.size(), 2);
    QVERIFY(down.first().key.has_value());
    QCOMPARE(int(*down.first().key), int(TerminalKey::Down));
    QCOMPARE(down.first().native.virtualKey, quint16(0x28));
    QVERIFY(TerminalInputRouting::alternateScrollKeys(0).isEmpty());
}

QTEST_MAIN(TerminalInputRoutingTest)
#include "terminal_input_routing_test.moc"
