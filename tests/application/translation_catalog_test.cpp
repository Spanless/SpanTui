#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <algorithm>
#include <utility>

namespace {

struct CatalogEntry {
    QString translation;
    bool unfinished = false;
};

struct SourceCatalog {
    QSet<QString> exactKeys;
    QSet<QString> sources;
};

// A brace scope of one C++ file. Only namespaces and classes matter for the
// context of a tr() call; everything else (function bodies, lambdas, brace
// initializers) is Other.
struct Scope {
    enum Kind { Namespace, Class, Other };
    Kind kind = Other;
    qsizetype start = 0;
    qsizetype end = -1;
    QString name;
    // Class: the context its tr() uses at runtime — the moc name with
    // namespaces for Q_OBJECT/Q_GADGET, the Q_DECLARE_TR_FUNCTIONS argument,
    // or empty when the class has no tr() of its own.
    QString trContext;
};

struct CppFile {
    QString path;
    QString text;
    QString code;
    QVector<Scope> scopes;
};

using Catalog = QHash<QString, CatalogEntry>;

QString catalogKey(const QString &context, const QString &source)
{
    return context + QChar(0x1f) + source;
}

QString displayKey(const QString &key)
{
    const int separator = key.indexOf(QChar(0x1f));
    if (separator < 0) return key;
    return key.left(separator) + QStringLiteral(" :: ") + key.mid(separator + 1);
}

Catalog readCatalog(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return {};
    }

    QXmlStreamReader xml(&file);
    Catalog catalog;
    QString context;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;

        if (xml.name() == QLatin1String("context")) {
            context.clear();
            continue;
        }
        if (xml.name() == QLatin1String("name") && context.isEmpty()) {
            context = xml.readElementText();
            continue;
        }
        if (xml.name() != QLatin1String("message")) continue;

        QString source;
        CatalogEntry entry;
        while (!(xml.isEndElement() && xml.name() == QLatin1String("message")) && !xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == QLatin1String("source")) {
                source = xml.readElementText(QXmlStreamReader::IncludeChildElements);
            } else if (xml.name() == QLatin1String("translation")) {
                entry.unfinished = xml.attributes().value(QLatin1String("type")) == QLatin1String("unfinished");
                entry.translation = xml.readElementText(QXmlStreamReader::IncludeChildElements);
            }
        }
        if (!source.isEmpty()) catalog.insert(catalogKey(context, source), entry);
    }

    if (xml.hasError()) *error = xml.errorString();
    return catalog;
}

QString decodeEscapedLiteral(const QString &literal)
{
    QString result;
    result.reserve(literal.size());
    for (qsizetype i = 0; i < literal.size(); ++i) {
        if (literal.at(i) != QLatin1Char('\\') || i + 1 >= literal.size()) {
            result += literal.at(i);
            continue;
        }

        const QChar escaped = literal.at(++i);
        if (escaped == QLatin1Char('n')) result += QLatin1Char('\n');
        else if (escaped == QLatin1Char('r')) result += QLatin1Char('\r');
        else if (escaped == QLatin1Char('t')) result += QLatin1Char('\t');
        else if (escaped == QLatin1Char('b')) result += QLatin1Char('\b');
        else if (escaped == QLatin1Char('f')) result += QLatin1Char('\f');
        else if (escaped == QLatin1Char('v')) result += QChar(0x0b);
        else if (escaped == QLatin1Char('u') && i + 4 < literal.size()) {
            bool ok = false;
            const ushort value = literal.mid(i + 1, 4).toUShort(&ok, 16);
            if (ok) {
                result += QChar(value);
                i += 4;
            } else {
                result += escaped;
            }
        } else {
            result += escaped;
        }
    }
    return result;
}

QString decodeLiteralSequence(const QString &expression)
{
    static const QRegularExpression literal(QStringLiteral("\"((?:\\\\.|[^\"\\\\])*)\""));
    QString result;
    auto matches = literal.globalMatch(expression);
    while (matches.hasNext()) result += decodeEscapedLiteral(matches.next().captured(1));
    return result;
}

QString readTextFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool writeTextFile(const QString &path, const QByteArray &contents, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(contents) != contents.size()) {
        *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

void collectLiteralCalls(const QString &text,
                         const QRegularExpression &expression,
                         const QString &context,
                         SourceCatalog *catalog)
{
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString source = decodeLiteralSequence(match.captured(1));
        if (source.isEmpty()) continue;
        catalog->sources.insert(source);
        if (!context.isEmpty()) catalog->exactKeys.insert(catalogKey(context, source));
    }
}

bool isIdentifierChar(QChar ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

// The file with comment text and the contents of string/char literals
// replaced by spaces (quotes and newlines kept), so braces and calls can be
// found by offset without tripping over text.
QString codeOnly(const QString &text)
{
    QString code = text;
    const qsizetype size = text.size();
    const auto blank = [&code, size](qsizetype from, qsizetype to) {
        for (qsizetype k = from; k < to && k < size; ++k) {
            if (code.at(k) != QLatin1Char('\n')) code[k] = QLatin1Char(' ');
        }
    };
    qsizetype i = 0;
    while (i < size) {
        const QChar ch = text.at(i);
        const QChar next = i + 1 < size ? text.at(i + 1) : QChar();
        if (ch == QLatin1Char('/') && next == QLatin1Char('/')) {
            const qsizetype eol = text.indexOf(QLatin1Char('\n'), i);
            const qsizetype end = eol < 0 ? size : eol;
            blank(i, end);
            i = end;
        } else if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            const qsizetype close = text.indexOf(QStringLiteral("*/"), i + 2);
            const qsizetype end = close < 0 ? size : close + 2;
            blank(i, end);
            i = end;
        } else if (ch == QLatin1Char('R') && next == QLatin1Char('"')
                   && (i == 0 || !isIdentifierChar(text.at(i - 1))
                       || QStringLiteral("uUL8").contains(text.at(i - 1)))) {
            const qsizetype open = text.indexOf(QLatin1Char('('), i + 2);
            if (open < 0) break;
            const QString closing = QLatin1Char(')') + text.mid(i + 2, open - i - 2) + QLatin1Char('"');
            const qsizetype close = text.indexOf(closing, open);
            const qsizetype end = close < 0 ? size : close + closing.size();
            blank(i + 2, end - 1);
            i = end;
        } else if (ch == QLatin1Char('"')
                   || (ch == QLatin1Char('\'') && !(i > 0 && text.at(i - 1).isDigit()))) {
            qsizetype k = i + 1;
            while (k < size && text.at(k) != ch && text.at(k) != QLatin1Char('\n')) {
                if (text.at(k) == QLatin1Char('\\')) ++k;
                ++k;
            }
            blank(i + 1, k);
            i = k + 1;
        } else {
            ++i;
        }
    }
    return code;
}

QVector<Scope> scanScopes(const QString &code)
{
    static const QRegularExpression namespaceHeader(QStringLiteral(R"(\bnamespace\s*([\w:]*)\s*$)"));
    static const QRegularExpression externHeader(QStringLiteral(R"(\bextern\s*"\s*"\s*$)"));
    static const QRegularExpression enumHeader(QStringLiteral(R"(\benum\b)"));
    static const QRegularExpression classHeader(QStringLiteral(
        R"((?:^|[\s;}>])(?:class|struct)\s+(?:[A-Z][A-Z0-9_]*\s+)?(\w+)(?:\s+final)?\s*(?::[^;{}]*)?$)"));
    static const QRegularExpression declaredTr(QStringLiteral(R"(\bQ_DECLARE_TR_FUNCTIONS\s*\(\s*(\w+)\s*\))"));
    static const QRegularExpression metaObject(QStringLiteral(R"(\bQ_(?:OBJECT|GADGET)\b)"));

    QVector<Scope> scopes;
    QVector<int> open;
    QHash<int, QString> qualified;
    qsizetype segmentStart = 0;
    for (qsizetype i = 0; i < code.size(); ++i) {
        const QChar ch = code.at(i);
        if (ch == QLatin1Char(';')) {
            segmentStart = i + 1;
        } else if (ch == QLatin1Char('}')) {
            if (!open.isEmpty()) scopes[open.takeLast()].end = i;
            segmentStart = i + 1;
        } else if (ch == QLatin1Char('{')) {
            const QString header = code.mid(segmentStart, i - segmentStart);
            Scope scope;
            scope.start = i;
            if (const auto match = namespaceHeader.match(header); match.hasMatch()) {
                scope.kind = Scope::Namespace;
                scope.name = match.captured(1);
            } else if (externHeader.match(header).hasMatch()) {
                scope.kind = Scope::Namespace;
            } else if (!enumHeader.match(header).hasMatch()) {
                if (const auto match = classHeader.match(header); match.hasMatch()) {
                    scope.kind = Scope::Class;
                    scope.name = match.captured(1);
                }
            }
            if (scope.kind == Scope::Class) {
                QStringList path;
                for (const int index : std::as_const(open)) {
                    if (scopes.at(index).kind != Scope::Other && !scopes.at(index).name.isEmpty())
                        path << scopes.at(index).name;
                }
                path << scope.name;
                qualified.insert(int(scopes.size()), path.join(QStringLiteral("::")));
            }
            scopes.push_back(scope);
            open.push_back(int(scopes.size() - 1));
            segmentStart = i + 1;
        }
    }
    for (int index = 0; index < scopes.size(); ++index) {
        Scope &scope = scopes[index];
        if (scope.kind != Scope::Class || scope.end < 0) continue;
        const QString body = code.mid(scope.start, scope.end - scope.start);
        if (const auto match = declaredTr.match(body); match.hasMatch())
            scope.trContext = match.captured(1);
        else if (metaObject.match(body).hasMatch())
            scope.trContext = qualified.value(index);
    }
    return scopes;
}

// True when the "(" at `open` starts the parameter list of a function
// definition: after the matching ")" and any qualifiers comes the body "{" or
// a constructor initializer ":", not ";" or more expression.
bool startsDefinition(const QString &code, qsizetype open)
{
    static const QRegularExpression tail(QStringLiteral(R"(\G\s*(?:(?:const|noexcept|override|final)\b\s*)*([{:]))"));
    int depth = 0;
    for (qsizetype i = open; i < code.size(); ++i) {
        if (code.at(i) == QLatin1Char('(')) ++depth;
        else if (code.at(i) == QLatin1Char(')') && --depth == 0) {
            const auto match = tail.match(code, i + 1);
            return match.hasMatch()
                && !(match.captured(1) == QLatin1String(":")
                     && code.mid(match.capturedEnd(1), 1) == QLatin1String(":"));
        }
    }
    return false;
}

int lineOf(const QString &text, qsizetype offset)
{
    return int(text.left(offset).count(QLatin1Char('\n'))) + 1;
}

// The context Qt uses for the tr() call at `offset`: the innermost enclosing
// class, else the class of the member function defined at namespace scope
// around the call. `explicitClass` is set for Class::tr(...).
QString trContextAt(const CppFile &file,
                    qsizetype offset,
                    const QString &explicitClass,
                    const QHash<QString, QSet<QString>> &contextsByClass,
                    QString *problem)
{
    static const QRegularExpression definition(QStringLiteral(R"(\b(\w+)::~?\w+\s*\()"));
    const auto contextOf = [&](const QString &className) -> QString {
        const QSet<QString> contexts = contextsByClass.value(className);
        if (contexts.size() == 1) return *contexts.cbegin();
        if (contexts.isEmpty() && className.startsWith(QLatin1Char('Q'))) return className;
        *problem = contexts.isEmpty()
            ? QStringLiteral("class %1 has no tr() context").arg(className)
            : QStringLiteral("class name %1 is ambiguous").arg(className);
        return {};
    };
    if (!explicitClass.isEmpty()) return contextOf(explicitClass);

    const auto containing = [&file](qsizetype position) {
        QVector<const Scope *> result;
        for (const Scope &scope : file.scopes) {
            if (scope.start < position && (scope.end < 0 || position < scope.end))
                result.push_back(&scope);
        }
        return result;   // outermost first: scopes are recorded in opening order
    };
    const QVector<const Scope *> around = containing(offset);
    for (auto it = around.crbegin(); it != around.crend(); ++it) {
        if ((*it)->kind == Scope::Class) {
            if ((*it)->trContext.isEmpty())
                *problem = QStringLiteral("class %1 has no tr() context").arg((*it)->name);
            return (*it)->trContext;
        }
        if ((*it)->kind == Scope::Namespace) break;
    }
    if (around.isEmpty() || around.constLast()->kind == Scope::Namespace) {
        *problem = QStringLiteral("tr() outside any function");
        return {};
    }
    QString owner;
    auto definitions = definition.globalMatch(file.code);
    while (definitions.hasNext()) {
        const auto match = definitions.next();
        if (match.capturedStart() >= offset) break;
        const QVector<const Scope *> outer = containing(match.capturedStart());
        const bool namespaceLevel = std::all_of(outer.cbegin(), outer.cend(), [](const Scope *scope) {
            return scope->kind == Scope::Namespace;
        });
        if (namespaceLevel && startsDefinition(file.code, match.capturedEnd() - 1))
            owner = match.captured(1);
    }
    if (owner.isEmpty()) {
        *problem = QStringLiteral("tr() outside a member function");
        return {};
    }
    return contextOf(owner);
}

SourceCatalog scanFirstPartySources(const QString &sourceRoot, QString *error)
{
    static const QString stringSequence = QStringLiteral("((?:\"(?:\\\\.|[^\"\\\\])*\"\\s*)+)");
    const QRegularExpression qmlCall(QStringLiteral("\\bqsTr\\s*\\(\\s*") + stringSequence);
    const QRegularExpression cppCall(QStringLiteral(R"((?:\b(\w+)::|(?<![\w.>:]))tr\s*\(\s*")"));
    const QRegularExpression literalSequence(stringSequence);
    const QRegularExpression explicitCall(
        QStringLiteral("QCoreApplication::translate\\s*\\(\\s*\"((?:\\\\.|[^\"\\\\])*)\"\\s*,\\s*")
        + stringSequence);
    const QRegularExpression qmlTranslatorPragma(
        QStringLiteral("\\bpragma\\s+Translator\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\""));

    SourceCatalog catalog;
    const QString qmlRoot = QDir(sourceRoot).filePath(QStringLiteral("qml"));
    QDirIterator qmlFiles(qmlRoot, {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
    while (qmlFiles.hasNext()) {
        const QString path = qmlFiles.next();
        const QString text = readTextFile(path, error);
        if (!error->isEmpty()) return {};
        const auto pragmaMatch = qmlTranslatorPragma.match(text);
        const QString context = pragmaMatch.hasMatch()
            ? decodeEscapedLiteral(pragmaMatch.captured(1))
            : QFileInfo(path).completeBaseName();
        collectLiteralCalls(text, qmlCall, context, &catalog);
    }

    // C++: tr() resolves its context at runtime from the class it belongs
    // to, so every call is mapped to that class — a catalog entry filed under
    // another context (e.g. the class name without its namespace) is never
    // used and shows up as missing plus stale.
    QVector<CppFile> files;
    QHash<QString, QSet<QString>> contextsByClass;
    const QString cppRoot = QDir(sourceRoot).filePath(QStringLiteral("src"));
    QDirIterator cppFiles(cppRoot,
                          {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                          QDir::Files,
                          QDirIterator::Subdirectories);
    while (cppFiles.hasNext()) {
        const QString path = cppFiles.next();
        if (QDir::fromNativeSeparators(path).contains(QStringLiteral("/third_party/"))) continue;
        CppFile file;
        file.path = path;
        file.text = readTextFile(path, error);
        if (!error->isEmpty()) return {};
        file.code = codeOnly(file.text);
        file.scopes = scanScopes(file.code);
        for (const Scope &scope : std::as_const(file.scopes)) {
            if (scope.kind == Scope::Class && !scope.trContext.isEmpty())
                contextsByClass[scope.name].insert(scope.trContext);
        }
        files.push_back(std::move(file));
    }

    QStringList unresolved;
    for (const CppFile &file : std::as_const(files)) {
        auto calls = cppCall.globalMatch(file.code);
        while (calls.hasNext()) {
            const auto call = calls.next();
            const qsizetype quote = call.capturedEnd() - 1;
            const auto literal = literalSequence.match(file.text, quote, QRegularExpression::NormalMatch,
                                                       QRegularExpression::AnchorAtOffsetMatchOption);
            const QString source = literal.hasMatch() ? decodeLiteralSequence(literal.captured(1)) : QString();
            if (source.isEmpty()) continue;
            catalog.sources.insert(source);
            QString problem;
            const QString context = trContextAt(file, call.capturedStart(), call.captured(1),
                                                contextsByClass, &problem);
            if (context.isEmpty()) {
                unresolved << QStringLiteral("%1:%2: %3 (\"%4\")")
                                  .arg(QDir(sourceRoot).relativeFilePath(file.path))
                                  .arg(lineOf(file.text, call.capturedStart()))
                                  .arg(problem, source);
                continue;
            }
            catalog.exactKeys.insert(catalogKey(context, source));
        }

        auto translations = explicitCall.globalMatch(file.text);
        while (translations.hasNext()) {
            const auto match = translations.next();
            const QString context = decodeEscapedLiteral(match.captured(1));
            const QString source = decodeLiteralSequence(match.captured(2));
            if (context.isEmpty() || source.isEmpty()) continue;
            catalog.sources.insert(source);
            catalog.exactKeys.insert(catalogKey(context, source));
        }
    }
    if (!unresolved.isEmpty()) {
        unresolved.sort();
        *error = QStringLiteral("Cannot resolve the translation context of: %1")
                     .arg(unresolved.join(QStringLiteral(" | ")));
        return {};
    }
    return catalog;
}

QStringList placeholders(const QString &text)
{
    static const QRegularExpression expression(QStringLiteral("%(?:L?[1-9][0-9]*|n)"));
    QStringList result;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) result.push_back(matches.next().captured());
    result.sort();
    return result;
}

QString joinedDisplayKeys(const QSet<QString> &keys)
{
    QStringList display;
    for (const QString &key : keys) display.push_back(displayKey(key));
    display.sort();
    return display.join(QStringLiteral(" | "));
}

QString sourceOfKey(const QString &key)
{
    return key.mid(key.indexOf(QChar(0x1f)) + 1);
}

QSet<QString> sourcesOfKeys(const QSet<QString> &keys)
{
    QSet<QString> sources;
    for (const QString &key : keys) sources.insert(sourceOfKey(key));
    return sources;
}

// One line per unfinished or empty translation, and per translation whose
// placeholders differ from its source's.
QStringList translationProblems(const Catalog &catalog)
{
    QStringList problems;
    for (auto it = catalog.cbegin(); it != catalog.cend(); ++it) {
        const QString key = displayKey(it.key());
        if (it->unfinished) problems.push_back(QStringLiteral("Unfinished translation: %1").arg(key));
        if (it->translation.trimmed().isEmpty()) problems.push_back(QStringLiteral("Empty translation: %1").arg(key));
        if (placeholders(it->translation) != placeholders(sourceOfKey(it.key())))
            problems.push_back(QStringLiteral("Placeholders differ: %1").arg(key));
    }
    return problems;
}

} // namespace

class TranslationCatalogTest : public QObject
{
    Q_OBJECT

private slots:
    void sourceScannerRecognizesSupportedCalls();
    void sourceScannerRejectsUnownedTr();
    void catalogIsFreshAndComplete();
};

void TranslationCatalogTest::sourceScannerRecognizesSupportedCalls()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("qml/components")));
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("src/application")));

    QString error;
    const QString qmlPath = QDir(root.path()).filePath(QStringLiteral("qml/components/TestPane.qml"));
    const QString sharedQmlPath =
        QDir(root.path()).filePath(QStringLiteral("qml/components/SharedPane.qml"));
    const QString headerPath = QDir(root.path()).filePath(QStringLiteral("src/application/example.h"));
    const QString cppPath = QDir(root.path()).filePath(QStringLiteral("src/application/example.cpp"));
    QVERIFY2(writeTextFile(qmlPath,
                           "Item { property string label: qsTr(\"Line\\n%1\") }\n",
                           &error),
             qPrintable(error));
    QVERIFY2(writeTextFile(sharedQmlPath,
                           "pragma Translator: \"SharedContext\"\n"
                           "Item { property string label: qsTr(\"Shared text\") }\n",
                           &error),
             qPrintable(error));
    QVERIFY2(writeTextFile(headerPath,
                           "namespace demo::app {\n"
                           "class Example final : public QObject {\n"
                           "    Q_OBJECT\n"
                           "public:\n"
                           "    enum class Mode { A, B };\n"
                           "    QString inlineText() const { return tr(\"Inline text\"); }\n"
                           "};\n"
                           "struct Plain {\n"
                           "    Q_DECLARE_TR_FUNCTIONS(PlainContext)\n"
                           "public:\n"
                           "    static QString text() { return tr(\"Declared text\"); }\n"
                           "};\n"
                           "} // namespace demo::app\n",
                           &error),
             qPrintable(error));
    QVERIFY2(writeTextFile(cppPath,
                           "namespace demo::app {\n"
                           "namespace {\n"
                           "// tr(\"Commented out\") { brace in a comment\n"
                           "const char *kBrace = \"}\";\n"
                           "}\n"
                           "Example::Example(QObject *parent) : QObject(parent), m_items{1, 2}\n"
                           "{\n"
                           "    connect(this, &QObject::destroyed, this, [this]() {\n"
                           "        QFileInfo::exists(QStringLiteral(\"x\"));\n"
                           "        auto one = tr(\"Adjacent \"\n"
                           "                      \"literals %1\");\n"
                           "    });\n"
                           "}\n"
                           "QString helperText() { return Example::tr(\"Qualified text\"); }\n"
                           "} // namespace demo::app\n"
                           "auto two = QCoreApplication::translate(\"ExplicitContext\", \"Explicit text\");\n",
                           &error),
             qPrintable(error));

    const SourceCatalog source = scanFirstPartySources(root.path(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(source.exactKeys.contains(catalogKey(QStringLiteral("TestPane"),
                                                 QStringLiteral("Line\n%1"))));
    QVERIFY(source.exactKeys.contains(catalogKey(QStringLiteral("SharedContext"),
                                                 QStringLiteral("Shared text"))));
    QVERIFY(!source.exactKeys.contains(catalogKey(QStringLiteral("SharedPane"),
                                                  QStringLiteral("Shared text"))));
    QVERIFY(source.exactKeys.contains(catalogKey(QStringLiteral("ExplicitContext"),
                                                 QStringLiteral("Explicit text"))));
    // tr() takes the moc name of its class, namespace included.
    const QString example = QStringLiteral("demo::app::Example");
    QVERIFY(source.exactKeys.contains(catalogKey(example, QStringLiteral("Adjacent literals %1"))));
    QVERIFY(source.exactKeys.contains(catalogKey(example, QStringLiteral("Inline text"))));
    QVERIFY(source.exactKeys.contains(catalogKey(example, QStringLiteral("Qualified text"))));
    QVERIFY(!source.exactKeys.contains(catalogKey(QStringLiteral("Example"),
                                                  QStringLiteral("Adjacent literals %1"))));
    QVERIFY(source.exactKeys.contains(catalogKey(QStringLiteral("PlainContext"),
                                                 QStringLiteral("Declared text"))));
    QVERIFY(!source.sources.contains(QStringLiteral("Commented out")));
}

void TranslationCatalogTest::sourceScannerRejectsUnownedTr()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("qml")));
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("src")));
    QString error;
    QVERIFY2(writeTextFile(QDir(root.path()).filePath(QStringLiteral("src/free.cpp")),
                           "namespace demo {\n"
                           "QString orphan() { return tr(\"Orphan text\"); }\n"
                           "}\n",
                           &error),
             qPrintable(error));

    scanFirstPartySources(root.path(), &error);
    QVERIFY(error.contains(QStringLiteral("free.cpp:2")));
    QVERIFY(error.contains(QStringLiteral("Orphan text")));
}

void TranslationCatalogTest::catalogIsFreshAndComplete()
{
    const QString sourceRoot = QStringLiteral(SPANTUI_SOURCE_DIR);
    const QString committedPath = QDir(sourceRoot).filePath(QStringLiteral("translations/SpanTui_zh_CN.ts"));

    QString committedError;
    QString sourceError;
    const Catalog committed = readCatalog(committedPath, &committedError);
    const SourceCatalog source = scanFirstPartySources(sourceRoot, &sourceError);
    QVERIFY2(committedError.isEmpty(), qPrintable(committedError));
    QVERIFY2(sourceError.isEmpty(), qPrintable(sourceError));

    const QSet<QString> committedKeys(committed.keyBegin(), committed.keyEnd());
    const QSet<QString> committedSources = sourcesOfKeys(committedKeys);

    const QSet<QString> missingExact = source.exactKeys - committedKeys;
    const QSet<QString> missingSources = source.sources - committedSources;
    QVERIFY2(missingExact.isEmpty(),
             qPrintable(QStringLiteral("Catalog is missing contextual entries: %1")
                            .arg(joinedDisplayKeys(missingExact))));
    QVERIFY2(missingSources.isEmpty(),
             qPrintable(QStringLiteral("Catalog is missing source strings: %1")
                            .arg(QStringList(missingSources.cbegin(), missingSources.cend())
                                     .join(QStringLiteral(" | ")))));

    // Every QML and C++ string has an exact key, so an entry under any other
    // context (or for a removed string) is never used at runtime.
    const QSet<QString> stale = committedKeys - source.exactKeys;
    QVERIFY2(stale.isEmpty(),
             qPrintable(QStringLiteral("Catalog contains stale entries: %1")
                            .arg(joinedDisplayKeys(stale))));

    const QStringList problems = translationProblems(committed);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral(" | "))));
}

QTEST_APPLESS_MAIN(TranslationCatalogTest)

#include "translation_catalog_test.moc"
