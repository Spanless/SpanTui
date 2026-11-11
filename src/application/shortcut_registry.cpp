#include "application/shortcut_registry.h"

#include "application/workspace_settings.h"
#include "terminal/terminal_constants.h"

#include <QStringList>

namespace span::application {

namespace {

constexpr Qt::KeyboardModifiers kChordModifiers =
    Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;

QString scopeName(ShortcutRegistry::Scope scope)
{
    switch (scope) {
    case ShortcutRegistry::Scope::Global: return QStringLiteral("global");
    case ShortcutRegistry::Scope::Terminal: return QStringLiteral("terminal");
    case ShortcutRegistry::Scope::Editor: return QStringLiteral("editor");
    case ShortcutRegistry::Scope::GitLog: return QStringLiteral("gitLog");
    }
    return {};
}

bool isModifierKey(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt
        || key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_Super_L
        || key == Qt::Key_Super_R || key == Qt::Key_unknown || key == 0;
}

} // namespace

ShortcutRegistry::ShortcutRegistry(WorkspaceSettings *state, QObject *parent) : QObject(parent), m_state(state)
{
    if (m_state) {
        connect(m_state, &WorkspaceSettings::shortcutOverridesChanged, this, [this]() {
            ++m_revision;
            emit bindingsChanged();
        });
    }
}

const QVector<ShortcutRegistry::Action> &ShortcutRegistry::actions()
{
    static const QVector<Action> list{
        {"terminal.find", "terminal", Scope::Terminal, "Ctrl+Shift+F"},
        {span::terminal::kActionCopy.data(), "terminal", Scope::Terminal, "Ctrl+Shift+C"},
        {span::terminal::kActionPaste.data(), "terminal", Scope::Terminal, "Ctrl+Shift+V"},
        {span::terminal::kActionPreviousPrompt.data(), "terminal", Scope::Terminal, "Ctrl+Shift+Up"},
        {span::terminal::kActionNextPrompt.data(), "terminal", Scope::Terminal, "Ctrl+Shift+Down"},
        {"editor.find", "editor", Scope::Editor, "Ctrl+F"},
        {"editor.goToLine", "editor", Scope::Editor, "Ctrl+G"},
        {"editor.copyLine", "editor", Scope::Editor, "Ctrl+C"},
        {"git.copyHash", "git", Scope::GitLog, "Ctrl+C"},
    };
    return list;
}

const ShortcutRegistry::Action *ShortcutRegistry::find(const QString &id)
{
    for (const Action &action : actions()) {
        if (id == QLatin1String(action.id)) {
            return &action;
        }
    }
    return nullptr;
}

bool ShortcutRegistry::isAcceptable(const QKeySequence &sequence)
{
    if (sequence.count() != 1) {
        return false;
    }
    const QKeyCombination chord = sequence[0];
    const int key = chord.key();
    if (isModifierKey(key)) {
        return false;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return true;
    }
    return chord.keyboardModifiers().testAnyFlags(Qt::ControlModifier | Qt::AltModifier);
}

bool ShortcutRegistry::scopesOverlap(Scope left, Scope right)
{
    return left == right || left == Scope::Global || right == Scope::Global;
}

QKeySequence ShortcutRegistry::effective(const Action &action) const
{
    const QString id = QString::fromLatin1(action.id);
    if (m_state) {
        const QVariantMap overrides = m_state->shortcutOverrides();
        const auto it = overrides.constFind(id);
        if (it != overrides.constEnd()) {
            const QString stored = it.value().toString();
            if (stored.isEmpty()) {
                return {};
            }
            const QKeySequence parsed = QKeySequence::fromString(stored, QKeySequence::PortableText);
            if (isAcceptable(parsed)) {
                return parsed;
            }
        }
    }
    return QKeySequence::fromString(QString::fromLatin1(action.defaultSequence),
                                    QKeySequence::PortableText);
}

QString ShortcutRegistry::sequence(const QString &id) const
{
    const Action *action = find(id);
    return action ? effective(*action).toString(QKeySequence::PortableText) : QString();
}

QString ShortcutRegistry::nativeText(const QString &id) const
{
    const Action *action = find(id);
    return action ? effective(*action).toString(QKeySequence::NativeText) : QString();
}

QString ShortcutRegistry::defaultSequence(const QString &id) const
{
    const Action *action = find(id);
    return action ? QString::fromLatin1(action->defaultSequence) : QString();
}

QVariantList ShortcutRegistry::catalog() const
{
    QVariantList result;
    for (const Action &action : actions()) {
        const QKeySequence current = effective(action);
        QStringList sharedWith;
        for (const Action &other : actions()) {
            if (&other != &action && !current.isEmpty() && scopesOverlap(other.scope, action.scope)
                && effective(other) == current) {
                sharedWith.append(QString::fromLatin1(other.id));
            }
        }
        const QKeySequence fallback = QKeySequence::fromString(
            QString::fromLatin1(action.defaultSequence), QKeySequence::PortableText);
        result.push_back(QVariantMap{
            {QStringLiteral("id"), QString::fromLatin1(action.id)},
            {QStringLiteral("group"), QString::fromLatin1(action.group)},
            {QStringLiteral("scope"), scopeName(action.scope)},
            {QStringLiteral("sequence"), current.toString(QKeySequence::PortableText)},
            {QStringLiteral("nativeText"), current.toString(QKeySequence::NativeText)},
            {QStringLiteral("defaultSequence"), fallback.toString(QKeySequence::PortableText)},
            {QStringLiteral("defaultNativeText"), fallback.toString(QKeySequence::NativeText)},
            {QStringLiteral("customized"), current != fallback},
            {QStringLiteral("sharedWith"), sharedWith},
        });
    }
    return result;
}

QVariantMap ShortcutRegistry::setBinding(const QString &id, const QString &sequenceText)
{
    const Action *action = find(id);
    if (!action) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("unknown")}};
    }
    QKeySequence requested;
    if (!sequenceText.isEmpty()) {
        requested = QKeySequence::fromString(sequenceText, QKeySequence::PortableText);
        if (!isAcceptable(requested)) {
            return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("invalid")}};
        }
    }
    if (!m_state) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("invalid")}};
    }
    QVariantMap overrides = m_state->shortcutOverrides();
    const QKeySequence fallback = QKeySequence::fromString(
        QString::fromLatin1(action->defaultSequence), QKeySequence::PortableText);
    if (requested == fallback) {
        overrides.remove(id);
    } else {
        overrides.insert(id, requested.toString(QKeySequence::PortableText));
    }
    m_state->setShortcutOverrides(overrides);
    return {{QStringLiteral("ok"), true}};
}

void ShortcutRegistry::resetBinding(const QString &id)
{
    if (!m_state || !find(id)) {
        return;
    }
    QVariantMap overrides = m_state->shortcutOverrides();
    overrides.remove(id);
    m_state->setShortcutOverrides(overrides);
}

void ShortcutRegistry::resetAll()
{
    if (m_state) {
        m_state->setShortcutOverrides({});
    }
}

QString ShortcutRegistry::sequenceFromKey(int key, int modifiers) const
{
    if (isModifierKey(key)) {
        return {};
    }
    const auto chordModifiers = Qt::KeyboardModifiers(modifiers) & kChordModifiers;
    return QKeySequence(QKeyCombination(chordModifiers, Qt::Key(key)))
        .toString(QKeySequence::PortableText);
}

bool ShortcutRegistry::matches(const QString &id, int key, int modifiers) const
{
    const Action *action = find(id);
    if (!action || isModifierKey(key)) {
        return false;
    }
    const QKeySequence bound = effective(*action);
    if (bound.isEmpty()) {
        return false;
    }
    const auto chordModifiers = Qt::KeyboardModifiers(modifiers) & kChordModifiers;
    return bound[0] == QKeyCombination(chordModifiers, Qt::Key(key));
}

QVariantMap ShortcutRegistry::terminalBindings() const
{
    QVariantMap result;
    for (const Action &action : actions()) {
        if (action.scope == Scope::Terminal) {
            result.insert(QString::fromLatin1(action.id),
                          effective(action).toString(QKeySequence::PortableText));
        }
    }
    return result;
}

} // namespace span::application
