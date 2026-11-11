#include "terminal/api/terminal_process.h"
#include "terminal/api/terminal_session.h"
#include "terminal/render/terminal_view.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace span::terminal;

class TerminalViewBindingTest final : public QObject {
    Q_OBJECT

private slots:
    void oldWindowCannotRefocusReboundView();
    void configuredShortcutsAreConsumedBeforeTheSession();
};

void TerminalViewBindingTest::oldWindowCannotRefocusReboundView() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(QStringLiteral("window-binding"), directory.path(), {}, nullptr,
                            []() { return std::unique_ptr<TerminalProcess>{}; });
    QQuickWindow oldWindow;
    QQuickWindow currentWindow;
    oldWindow.resize(320, 200);
    currentWindow.resize(320, 200);
    TerminalView view;
    view.setSession(&session);
    view.setSize(QSizeF(240, 120));
    view.setParentItem(oldWindow.contentItem());
    oldWindow.show();
    currentWindow.show();
    if (!QTest::qWaitForWindowExposed(&currentWindow))
        QSKIP("Window exposure unavailable; window-binding behavior not verified.");
    view.setParentItem(currentWindow.contentItem());
    QQuickItem otherFocusTarget(currentWindow.contentItem());
    otherFocusTarget.setSize(QSizeF(40, 40));
    currentWindow.requestActivate();
    if (!QTest::qWaitFor([&]() { return currentWindow.isActive(); }, 5000))
        QSKIP("Window activation unavailable; focus isolation not verified.");
    QTest::qWait(50);
    session.stop();
    otherFocusTarget.forceActiveFocus();
    QTRY_VERIFY(otherFocusTarget.hasActiveFocus());
    QVERIFY(QMetaObject::invokeMethod(&oldWindow, "activeChanged", Qt::DirectConnection));
    QTest::qWait(50);
    QVERIFY(otherFocusTarget.hasActiveFocus());
    QVERIFY(!view.hasActiveFocus());
    view.setParentItem(nullptr);
}

void TerminalViewBindingTest::configuredShortcutsAreConsumedBeforeTheSession() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TerminalSession session(QStringLiteral("shortcuts"), directory.path(), {}, nullptr,
                            []() { return std::unique_ptr<TerminalProcess>{}; });
    TerminalView view;
    view.setSession(&session);
    view.setShortcutBindings({{QStringLiteral("terminal.find"), QStringLiteral("Ctrl+Shift+F")},
                              {QStringLiteral("terminal.copy"), QStringLiteral("Ctrl+Shift+C")}});
    QSignalSpy activated(&view, &TerminalView::shortcutActivated);
    const auto press = [&view](int key, Qt::KeyboardModifiers modifiers, bool repeat = false) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), repeat);
        QCoreApplication::sendEvent(&view, &event);
        return event.isAccepted();
    };

    // Shortcuts work on a session that is not running: its history stays searchable.
    QVERIFY(press(Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier));
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), QStringLiteral("terminal.find"));
    // Copy applies only to a selection; without one the chord falls through
    // to the (stopped) session, which declines it, and no signal is sent.
    QVERIFY(!press(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier));
    QCOMPARE(activated.count(), 1);
    // Unbound chords fall through to the session as well.
    QVERIFY(!press(Qt::Key_F, Qt::ControlModifier));

    view.setShortcutBindings({{QStringLiteral("terminal.find"), QStringLiteral("Alt+F3")}});
    QVERIFY(!press(Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier));
    QVERIFY(press(Qt::Key_F3, Qt::AltModifier));
    QCOMPARE(activated.count(), 2);
    // Auto-repeat is swallowed instead of reopening the bar repeatedly.
    QVERIFY(press(Qt::Key_F3, Qt::AltModifier, true));
    QCOMPARE(activated.count(), 2);

    // A shared chord runs every action that applies: copy has no selection
    // here, so only find runs, and the press is still consumed.
    view.setShortcutBindings({{QStringLiteral("terminal.copy"), QStringLiteral("Ctrl+Shift+F")},
                              {QStringLiteral("terminal.find"), QStringLiteral("Ctrl+Shift+F")}});
    QVERIFY(press(Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier));
    QCOMPARE(activated.count(), 3);
    QCOMPARE(activated.at(2).at(0).toString(), QStringLiteral("terminal.find"));
}

QTEST_MAIN(TerminalViewBindingTest)
#include "terminal_view_binding_test.moc"
