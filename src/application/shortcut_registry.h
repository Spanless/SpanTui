#pragma once

#include <QKeySequence>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace span::application {

class WorkspaceSettings;

// Catalog and policy for configurable shortcuts. The user's overrides live in
// WorkspaceSettings::shortcutOverrides; this class owns the action list, the defaults
// and the rules a binding must satisfy, and resolves effective sequences.
// Titles are presentation and stay in QML; results carry ids and codes.
class ShortcutRegistry final : public QObject {
    Q_OBJECT
    // id -> portable sequence for the terminal scope, consumed by TerminalView
    // (the terminal library cannot depend on this layer).
    Q_PROPERTY(QVariantMap terminalBindings READ terminalBindings NOTIFY bindingsChanged FINAL)
    // id -> portable sequence of the application (global) actions. A focused
    // terminal leaves these chords to the application.
    Q_PROPERTY(QVariantMap applicationBindings READ applicationBindings NOTIFY bindingsChanged FINAL)
    // The system menu bar runs the application actions through its key
    // equivalents (macOS); elsewhere window shortcuts do.
    Q_PROPERTY(bool nativeMenuBar READ nativeMenuBar CONSTANT FINAL)
    // Bumped on every change so QML bindings that call sequence()/matches()
    // can depend on it.
    Q_PROPERTY(int revision READ revision NOTIFY bindingsChanged FINAL)

public:
    enum class Scope { Global, Terminal, Editor, GitLog };

    // Two defaults: where Ctrl chords belong to the shell, and where a
    // Command key exists (portable "Ctrl" is Command there).
    struct Action {
        const char *id;
        const char *group;
        Scope scope;
        const char *ctrlDefault;
        const char *commandDefault;
    };

    explicit ShortcutRegistry(WorkspaceSettings *state, QObject *parent = nullptr);

    static const QVector<Action> &actions();

    // Effective portable sequence ("Ctrl+Shift+F"), empty when unbound or unknown.
    Q_INVOKABLE QString sequence(const QString &id) const;
    // Platform display text of the effective sequence.
    Q_INVOKABLE QString nativeText(const QString &id) const;
    Q_INVOKABLE QString defaultSequence(const QString &id) const;
    // [{id, group, scope, sequence, nativeText, defaultSequence, defaultNativeText,
    //   customized, sharedWith}]. sharedWith lists the ids bound to the same
    // chord where both receive the key; a press runs each that applies.
    Q_INVOKABLE QVariantList catalog() const;
    // {ok: bool, error: "unknown"|"invalid"}. Chords may be shared.
    // An empty sequence unbinds the action.
    Q_INVOKABLE QVariantMap setBinding(const QString &id, const QString &sequence);
    Q_INVOKABLE void resetBinding(const QString &id);
    Q_INVOKABLE void resetAll();
    // `key` and `modifiers` are a QML KeyEvent's key and modifiers.
    Q_INVOKABLE bool matches(const QString &id, int key, int modifiers) const;
    // Portable text of one recorded chord; empty for a bare modifier press.
    Q_INVOKABLE QString sequenceFromKey(int key, int modifiers) const;

    QVariantMap terminalBindings() const;
    QVariantMap applicationBindings() const;
    bool nativeMenuBar() const;
    int revision() const { return m_revision; }

    // Single-chord sequence a binding may use: Ctrl or Alt held, or F1-F24.
    static bool isAcceptable(const QKeySequence &sequence);

signals:
    void bindingsChanged();

private:
    static const Action *find(const QString &id);
    static QKeySequence defaultOf(const Action &action);
    QVariantMap bindingsIn(Scope scope) const;
    QKeySequence effective(const Action &action) const;
    static bool scopesOverlap(Scope left, Scope right);

    WorkspaceSettings *m_state = nullptr;
    int m_revision = 0;
};

} // namespace span::application
