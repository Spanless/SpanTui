#include "git/git_repository_service.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>

namespace {
// Lane color slots; GitLogView.graphColor() wraps them onto Theme.gitGraph.
constexpr int kGraphColorCount = 8;

QString normalizedGitPath(QString path) {
    return path.replace('\\', '/');
}

QString decode(const QByteArray &bytes) {
    return QString::fromUtf8(bytes);
}

QString decodePatchPath(QByteArray value) {
    if (value.endsWith('\r')) value.chop(1);
    if (value == QByteArrayLiteral("/dev/null")) return {};
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.mid(1, value.size() - 2);
        QByteArray decoded;
        decoded.reserve(value.size());
        for (int index = 0; index < value.size(); ++index) {
            char ch = value.at(index);
            if (ch != '\\' || index + 1 >= value.size()) {
                decoded += ch;
                continue;
            }
            ch = value.at(++index);
            if (ch >= '0' && ch <= '7') {
                int octal = ch - '0';
                int digits = 1;
                while (digits < 3 && index + 1 < value.size()
                       && value.at(index + 1) >= '0' && value.at(index + 1) <= '7') {
                    octal = octal * 8 + value.at(++index) - '0';
                    ++digits;
                }
                decoded += char(octal);
                continue;
            }
            switch (ch) {
            case 't': decoded += '\t'; break;
            case 'n': decoded += '\n'; break;
            case 'r': decoded += '\r'; break;
            case 'b': decoded += '\b'; break;
            case 'f': decoded += '\f'; break;
            case 'v': decoded += '\v'; break;
            default: decoded += ch; break;
            }
        }
        value = decoded;
    }
    QString path = decode(value);
    if (path.startsWith(QStringLiteral("a/")) || path.startsWith(QStringLiteral("b/")))
        path.remove(0, 2);
    return normalizedGitPath(path);
}

bool parseDiffGitHeader(const QByteArray &header, QByteArray &outA, QByteArray &outB) {
    if (header.isEmpty()) return false;
    if (header.startsWith('"')) {
        int pos = 1;
        while (pos < header.size()) {
            if (header.at(pos) == '"' && header.at(pos - 1) != '\\')
                break;
            ++pos;
        }
        if (pos >= header.size()) return false;
        outA = header.mid(0, pos + 1);
        int next = pos + 1;
        while (next < header.size() && header.at(next) == ' ') ++next;
        outB = header.mid(next).trimmed();
        return true;
    }
    int quotePos = header.indexOf(" \"");
    if (quotePos != -1) {
        outA = header.left(quotePos);
        outB = header.mid(quotePos + 1).trimmed();
        return true;
    }
    int sep = header.lastIndexOf(" b/");
    if (sep != -1) {
        outA = header.left(sep);
        outB = header.mid(sep + 1);
        return true;
    }
    return false;
}
}

QString GitRepositoryService::statusName(QChar code) {
    switch (code.toLatin1()) {
    case 'A': case '?': return QStringLiteral("added");
    case 'D': return QStringLiteral("deleted");
    case 'R': return QStringLiteral("renamed");
    case 'C': return QStringLiteral("copied");
    case 'U': return QStringLiteral("conflict");
    case 'T': return QStringLiteral("type-changed");
    default: return QStringLiteral("modified");
    }
}

int GitRepositoryService::statusPriority(const QString &status) {
    if (status == QStringLiteral("conflict")) return 6;
    if (status == QStringLiteral("deleted")) return 5;
    if (status == QStringLiteral("modified") || status == QStringLiteral("type-changed")) return 4;
    if (status == QStringLiteral("renamed") || status == QStringLiteral("copied")) return 3;
    if (status == QStringLiteral("added")) return 2;
    return 0;
}

QVariantMap GitRepositoryService::parseCommitPreview(const QByteArray &output)
{
    const qsizetype separator = output.indexOf('\0');
    const QString message = decode(separator < 0 ? output : output.left(separator));
    const QString stat = separator < 0 ? QString() : decode(output.mid(separator + 1));

    // The subject paragraph is already on the history row; the preview
    // carries what follows it. Co-author trailers become structured entries
    // instead of message text.
    static const QRegularExpression coAuthorLine(
        QStringLiteral(R"(^\s*co-authored-by:\s*(.*?)\s*(?:<([^>]*)>)?\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QStringList lines = message.split(QLatin1Char('\n'));
    QStringList bodyLines;
    QVariantList coAuthors;
    bool inSubject = true;
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (inSubject) {
            if (line.trimmed().isEmpty())
                inSubject = false;
            continue;
        }
        const QRegularExpressionMatch coAuthor = coAuthorLine.match(line);
        if (coAuthor.hasMatch()) {
            coAuthors.append(QVariantMap{{QStringLiteral("name"), coAuthor.captured(1)},
                                         {QStringLiteral("email"), coAuthor.captured(2)}});
            continue;
        }
        bodyLines.append(line);
    }
    QString body = bodyLines.join(QLatin1Char('\n')).trimmed();
    static const QRegularExpression blankRuns(QStringLiteral("\n{3,}"));
    body.replace(blankRuns, QStringLiteral("\n\n"));

    static const QRegularExpression shortstat(QStringLiteral(
        R"((\d+) files? changed(?:, (\d+) insertions?\(\+\))?(?:, (\d+) deletions?\(-\))?)"));
    const QRegularExpressionMatch totals = shortstat.match(stat);
    return {
        {QStringLiteral("body"), body},
        {QStringLiteral("coAuthors"), coAuthors},
        {QStringLiteral("hasStats"), totals.hasMatch()},
        {QStringLiteral("filesChanged"), totals.captured(1).toInt()},
        {QStringLiteral("insertions"), totals.captured(2).toInt()},
        {QStringLiteral("deletions"), totals.captured(3).toInt()},
    };
}

QString GitRepositoryService::cleanError(const QByteArray &stderrData) {
    QString value = decode(stderrData).trimmed();
    value.replace(QRegularExpression(QStringLiteral(R"((https?://)[^/@\s]+@)")), QStringLiteral("\\1"));
    return value;
}

QVariantMap GitRepositoryService::changeItem(const QString &path, const QString &oldPath,
                                             const QString &status, bool staged, bool untracked,
                                             bool conflict) {
    return {{QStringLiteral("path"), normalizedGitPath(path)},
            {QStringLiteral("oldPath"), normalizedGitPath(oldPath)},
            {QStringLiteral("name"), QFileInfo(path).fileName()},
            {QStringLiteral("status"), status},
            {QStringLiteral("staged"), staged},
            {QStringLiteral("untracked"), untracked},
            {QStringLiteral("conflict"), conflict},
            {QStringLiteral("additions"), 0}, {QStringLiteral("deletions"), 0}};
}

QVariantMap GitRepositoryService::parseStatus(const QByteArray &output, QVariantList *staged,
                                              QVariantList *working, QHash<QString, QString> *statuses) {
    QVariantMap state {{QStringLiteral("branch"), QString()}};
    const QList<QByteArray> records = output.split('\0');
    for (int i = 0; i < records.size(); ++i) {
        const QByteArray raw = records.at(i);
        if (raw.isEmpty()) continue;
        if (raw.startsWith("# branch.head ")) { state[QStringLiteral("branch")] = decode(raw.mid(14)); continue; }
        if (raw.startsWith("# branch.oid ")) { state[QStringLiteral("unborn")] = raw.mid(13) == "(initial)"; continue; }
        if (raw.startsWith("# ")) continue;
        if (raw.startsWith("? ")) {
            const QString path = decode(raw.mid(2));
            working->push_back(changeItem(path, {}, QStringLiteral("added"), false, true, false));
            statuses->insert(normalizedGitPath(path), QStringLiteral("added"));
            continue;
        }
        if (raw.startsWith("! ")) {
            QString path = decode(raw.mid(2));
            while (path.endsWith(QLatin1Char('/')))
                path.chop(1);
            if (!path.isEmpty())
                statuses->insert(normalizedGitPath(path), QStringLiteral("ignored"));
            continue;
        }
        if (raw.startsWith("u ")) {
            const QList<QByteArray> fields = raw.split(' ');
            if (fields.size() >= 11) {
                const QString path = decode(fields.mid(10).join(' '));
                staged->push_back(changeItem(path, {}, QStringLiteral("conflict"), true, false, true));
                working->push_back(changeItem(path, {}, QStringLiteral("conflict"), false, false, true));
                statuses->insert(normalizedGitPath(path), QStringLiteral("conflict"));
            }
            continue;
        }
        if (!raw.startsWith("1 ") && !raw.startsWith("2 ")) continue;
        const bool renamed = raw.startsWith("2 ");
        const QList<QByteArray> fields = raw.split(' ');
        const int pathIndex = renamed ? 9 : 8;
        if (fields.size() <= pathIndex) continue;
        const QByteArray xy = fields.at(1);
        const QString path = decode(fields.mid(pathIndex).join(' '));
        QString oldPath;
        if (renamed && i + 1 < records.size()) oldPath = decode(records.at(++i));
        const QChar indexCode = xy.size() > 0 ? QChar(xy.at(0)) : QChar('.');
        const QChar workCode = xy.size() > 1 ? QChar(xy.at(1)) : QChar('.');
        QString strongest;
        if (indexCode != '.') {
            const QString status = statusName(indexCode);
            staged->push_back(changeItem(path, oldPath, status, true, false, status == QStringLiteral("conflict")));
            strongest = status;
        }
        if (workCode != '.') {
            const QString status = statusName(workCode);
            working->push_back(changeItem(path, oldPath, status, false, false, status == QStringLiteral("conflict")));
            if (statusPriority(status) > statusPriority(strongest)) strongest = status;
        }
        if (!strongest.isEmpty()) statuses->insert(normalizedGitPath(path), strongest);
    }
    return state;
}

QVariantList GitRepositoryService::parseBranches(const QByteArray &output) {
    QVariantList result;
    for (const QByteArray &record : output.split('\n')) {
        if (record.trimmed().isEmpty()) continue;
        const QList<QByteArray> fields = record.split('\0');
        if (fields.size() < 6) continue;
        const QString fullRef = decode(fields.at(0));
        const QString name = decode(fields.at(1));
        if (fullRef.endsWith(QStringLiteral("/HEAD"))) continue;
        const bool remote = fullRef.startsWith(QStringLiteral("refs/remotes/"));
        const bool tag = fullRef.startsWith(QStringLiteral("refs/tags/"));
        const bool local = fullRef.startsWith(QStringLiteral("refs/heads/"));
        const QString kind = tag ? QStringLiteral("tag")
                                 : remote ? QStringLiteral("remote")
                                          : QStringLiteral("local");
        QString group = kind;
        if (remote) {
            const int slash = name.indexOf(QLatin1Char('/'));
            if (slash > 0) group = name.left(slash);
        }
        // `%(upstream:track)` compares with the local remote-tracking ref as
        // of the last fetch: "[ahead N, behind M]", "[gone]" when the
        // configured upstream ref no longer exists, or empty.
        int ahead = 0, behind = 0;
        const QString upstream = decode(fields.at(3));
        const QString track = decode(fields.at(4));
        static const QRegularExpression aheadPattern(QStringLiteral(R"(ahead (\d+))"));
        static const QRegularExpression behindPattern(QStringLiteral(R"(behind (\d+))"));
        QRegularExpressionMatch match = aheadPattern.match(track);
        if (match.hasMatch()) ahead = match.captured(1).toInt();
        match = behindPattern.match(track);
        if (match.hasMatch()) behind = match.captured(1).toInt();
        const bool upstreamGone = track == QStringLiteral("[gone]");
        result.push_back(QVariantMap{{QStringLiteral("fullRef"), fullRef},
                                     {QStringLiteral("name"), name},
                                     {QStringLiteral("kind"), kind},
                                     {QStringLiteral("group"), group},
                                     {QStringLiteral("objectId"), fields.size() > 6 ? decode(fields.at(6)) : QString()},
                                     // An annotated tag's own object id is not a commit; this is the
                                     // commit it peels to (empty for every other ref).
                                     {QStringLiteral("commitId"), fields.size() > 7 ? decode(fields.at(7)) : QString()},
                                     {QStringLiteral("remote"), remote},
                                     {QStringLiteral("local"), local},
                                     {QStringLiteral("current"), fields.at(2) == "*"},
                                     {QStringLiteral("upstream"), upstream},
                                     // A local branch with an upstream configured; with
                                     // upstreamGone its ref was deleted (pruned), so
                                     // ahead/behind are 0 and mean nothing.
                                     {QStringLiteral("hasUpstream"), local && !upstream.isEmpty()},
                                     {QStringLiteral("upstreamGone"), local && upstreamGone},
                                     {QStringLiteral("ahead"), ahead},
                                     {QStringLiteral("behind"), behind},
                                     {QStringLiteral("date"), decode(fields.at(5))}});
    }
    return result;
}

QVariantList GitRepositoryService::parseWorktrees(const QByteArray &output)
{
    // `git worktree list --porcelain`: one block per worktree, blank-line
    // separated, `key value` lines (`detached`, `bare`, `locked` and
    // `prunable` may carry no value). `-z` would be safer for exotic paths
    // but needs git 2.36; the supported floor is 2.16.
    QVariantList result;
    QVariantMap block;
    QString mainPath;
    const auto flush = [&result, &block, &mainPath]() {
        const QString rawPath = block.value(QStringLiteral("rawPath")).toString();
        if (!rawPath.isEmpty()) {
            const QString path = QDir::cleanPath(rawPath);
            const QString head = block.value(QStringLiteral("head")).toString();
            QString branch = block.value(QStringLiteral("branchRef")).toString();
            static const QString headsPrefix = QStringLiteral("refs/heads/");
            if (branch.startsWith(headsPrefix)) branch = branch.mid(headsPrefix.size());
            const QString leaf = QFileInfo(path).fileName();
            if (result.isEmpty()) mainPath = path;
            // Linked worktrees read shorter from the main one (a sibling is
            // `..\name`); the main one itself has no relative form.
            const QString relativePath =
                result.isEmpty() ? QString() : QDir::toNativeSeparators(QDir(mainPath).relativeFilePath(path));
            result.push_back(QVariantMap{
                {QStringLiteral("path"), path},
                {QStringLiteral("displayPath"), QDir::toNativeSeparators(path)},
                {QStringLiteral("relativePath"), relativePath},
                {QStringLiteral("name"), leaf.isEmpty() ? path : leaf},
                {QStringLiteral("branch"), branch},
                {QStringLiteral("head"), head.left(7)},
                {QStringLiteral("headId"), head},
                {QStringLiteral("detached"), block.value(QStringLiteral("detached")).toBool()},
                {QStringLiteral("bare"), block.value(QStringLiteral("bare")).toBool()},
                {QStringLiteral("locked"), block.value(QStringLiteral("locked")).toBool()},
                {QStringLiteral("lockReason"), block.value(QStringLiteral("lockReason")).toString()},
                {QStringLiteral("prunable"), block.value(QStringLiteral("prunable")).toBool()},
                {QStringLiteral("prunableReason"), block.value(QStringLiteral("prunableReason")).toString()},
                // git lists the main worktree first.
                {QStringLiteral("main"), result.isEmpty()},
                // The service marks the one it is reading.
                {QStringLiteral("current"), false}});
        }
        block.clear();
    };
    for (QByteArray line : output.split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) {
            flush();
            continue;
        }
        const int space = line.indexOf(' ');
        const QByteArray key = space < 0 ? line : line.left(space);
        const QString value = space < 0 ? QString() : decode(line.mid(space + 1));
        if (key == "worktree") {
            flush();
            block.insert(QStringLiteral("rawPath"), value);
        } else if (key == "HEAD") {
            block.insert(QStringLiteral("head"), value);
        } else if (key == "branch") {
            block.insert(QStringLiteral("branchRef"), value);
        } else if (key == "detached") {
            block.insert(QStringLiteral("detached"), true);
        } else if (key == "bare") {
            block.insert(QStringLiteral("bare"), true);
        } else if (key == "locked") {
            block.insert(QStringLiteral("locked"), true);
            block.insert(QStringLiteral("lockReason"), value);
        } else if (key == "prunable") {
            block.insert(QStringLiteral("prunable"), true);
            block.insert(QStringLiteral("prunableReason"), value);
        }
    }
    flush();
    return result;
}

namespace {
using HistoryGraphState = GitRepositoryService::HistoryGraph;

QVariantMap parseHistoryRecord(const QByteArray &raw)
{
    QByteArray record = raw.trimmed();
    if (record.isEmpty())
        return {};
    const QList<QByteArray> fields = record.split('\0');
    if (fields.size() < 8)
        return {};
    const QStringList parents = decode(fields.at(2)).split(' ', Qt::SkipEmptyParts);
    return {{QStringLiteral("hash"), decode(fields.at(0))},
            {QStringLiteral("shortHash"), decode(fields.at(1))},
            {QStringLiteral("parents"), parents},
            {QStringLiteral("author"), decode(fields.at(3))},
            {QStringLiteral("email"), decode(fields.at(4))},
            {QStringLiteral("date"), decode(fields.at(5))},
            {QStringLiteral("refs"), decode(fields.at(6))},
            {QStringLiteral("subject"), decode(fields.at(7))},
            {QStringLiteral("body"), fields.size() > 8
                                         ? decode(fields.mid(8).join("\0"))
                                         : QString()}};
}

QVariantMap decorateHistoryCommit(QVariantMap item, HistoryGraphState *state)
{
    const QString hash = item.value(QStringLiteral("hash")).toString();
    const QStringList parents = item.value(QStringLiteral("parents")).toStringList();

    int nodeLane = state->lanes.indexOf(hash);
    const bool isNewTip = nodeLane < 0;
    if (isNewTip) {
        nodeLane = state->lanes.size();
        state->lanes.push_back(hash);
        state->lineColor.insert(hash, state->colorCursor);
        state->colorCursor = (state->colorCursor + 1) % kGraphColorCount;
    }

    const QStringList before = state->lanes;
    // Lanes keep the colour they arrived with in this row, even when the
    // commit they lead to is recoloured below.
    QList<int> beforeColors;
    beforeColors.reserve(before.size());
    for (const QString &target : before)
        beforeColors.push_back(state->lineColor.value(target));
    QStringList after = before;
    after.removeAt(nodeLane);
    int insertLane = nodeLane;
    for (int parentIndex = 0; parentIndex < parents.size(); ++parentIndex) {
        const QString &parent = parents.at(parentIndex);
        int target = after.indexOf(parent);
        if (target < 0) {
            target = std::min(insertLane, int(after.size()));
            if (!state->lineColor.contains(parent)) {
                const int color = parentIndex == 0 ? state->lineColor.value(hash)
                                                   : state->colorCursor;
                state->lineColor.insert(parent, color);
                if (parentIndex != 0)
                    state->colorCursor = (state->colorCursor + 1) % kGraphColorCount;
            }
            after.insert(target, parent);
        } else if (parentIndex == 0 && target >= nodeLane) {
            // A branch to the right reached this first parent earlier. The
            // first-parent line wins: it stays in this column and keeps its
            // colour, and the side lane curves in to join it — otherwise the
            // main line would jog sideways and turn the side branch's colour.
            if (target > nodeLane) {
                after.removeAt(target);
                after.insert(nodeLane, parent);
                target = nodeLane;
            }
            state->lineColor.insert(parent, state->lineColor.value(hash));
        }
        insertLane = target + 1;
    }
    // Positions settle only after every parent is placed: a later insert
    // can shift an earlier parent's lane.
    QList<int> parentTargets;
    parentTargets.reserve(parents.size());
    for (const QString &parent : parents)
        parentTargets.push_back(int(after.indexOf(parent)));

    // Every segment names the commit its lane is heading for ("target"):
    // lanes are unique per target, so the view can trace one line of
    // descent across rows without re-deriving the lane assignment.
    QVariantList segments;
    if (!isNewTip) {
        segments.push_back(QVariantMap{{QStringLiteral("kind"), QStringLiteral("in")},
                                       {QStringLiteral("from"), nodeLane},
                                       {QStringLiteral("to"), nodeLane},
                                       {QStringLiteral("target"), hash},
                                       {QStringLiteral("color"), state->lineColor.value(hash)}});
    }
    for (int lane = 0; lane < before.size(); ++lane) {
        if (lane == nodeLane)
            continue;
        const int target = after.indexOf(before.at(lane));
        if (target < 0)
            continue;
        segments.push_back(QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("through")},
            {QStringLiteral("from"), lane},
            {QStringLiteral("to"), target},
            {QStringLiteral("target"), before.at(lane)},
            {QStringLiteral("color"), beforeColors.at(lane)}});
    }
    for (int parentIndex = 0; parentIndex < parentTargets.size(); ++parentIndex) {
        const int color = parentIndex == 0 ? state->lineColor.value(hash)
                                           : state->lineColor.value(parents.at(parentIndex));
        segments.push_back(QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("out")},
            {QStringLiteral("from"), nodeLane},
            {QStringLiteral("to"), parentTargets.at(parentIndex)},
            {QStringLiteral("target"), parents.at(parentIndex)},
            {QStringLiteral("color"), color}});
    }

    QVariantList decorations;
    const QStringList refs = item.value(QStringLiteral("refs")).toString()
                                 .split(QStringLiteral(", "), Qt::SkipEmptyParts);
    for (QString ref : refs) {
        ref = ref.trimmed();
        if (ref.isEmpty())
            continue;
        bool head = false;
        if (ref.startsWith(QStringLiteral("HEAD -> "))) {
            ref.remove(0, 8);
            head = true;
        }
        QString kind = QStringLiteral("local");
        if (ref.startsWith(QStringLiteral("tag: "))) {
            ref.remove(0, 5);
            kind = QStringLiteral("tag");
        } else if (ref.contains(QLatin1Char('/'))) {
            kind = QStringLiteral("remote");
        } else if (ref == QStringLiteral("HEAD")) {
            kind = QStringLiteral("head");
            head = true;
        }
        decorations.push_back(QVariantMap{{QStringLiteral("label"), ref},
                                          {QStringLiteral("kind"), kind},
                                          {QStringLiteral("head"), head}});
    }

    item.insert(QStringLiteral("nodeLane"), nodeLane);
    item.insert(QStringLiteral("nodeColor"), state->lineColor.value(hash));
    item.insert(QStringLiteral("laneCount"),
                std::max({1, int(before.size()), int(after.size())}));
    item.insert(QStringLiteral("graphSegments"), segments);
    item.insert(QStringLiteral("decorations"), decorations);
    state->lanes = after;
    return item;
}
} // namespace

QVariantList GitRepositoryService::parseHistoryPage(const QByteArray &output, HistoryGraph *graph)
{
    QVariantList rows;
    const QList<QByteArray> records = output.split('\x1e');
    rows.reserve(records.size());
    for (const QByteArray &record : records) {
        QVariantMap item = parseHistoryRecord(record);
        if (!item.isEmpty()) rows.push_back(decorateHistoryCommit(std::move(item), graph));
    }
    return rows;
}

QVariantList GitRepositoryService::parseNameStatus(const QByteArray &output) {
    QVariantList result;
    const QList<QByteArray> fields = output.split('\0');
    for (int i = 0; i < fields.size();) {
        if (fields.at(i).isEmpty()) { ++i; continue; }
        const QString code = decode(fields.at(i++));
        if (i >= fields.size()) break;
        QString oldPath, path = decode(fields.at(i++));
        if ((code.startsWith('R') || code.startsWith('C')) && i < fields.size()) {
            oldPath = path;
            path = decode(fields.at(i++));
        }
        const QString status = statusName(code.isEmpty() ? QChar('M') : code.at(0));
        result.push_back(changeItem(path, oldPath, status, false, false, status == QStringLiteral("conflict")));
    }
    return result;
}

namespace {
QString resolveNumstatPath(const QString &rawPath) {
    int arrow = rawPath.indexOf(QStringLiteral(" => "));
    if (arrow == -1) return rawPath;
    int braceStart = rawPath.indexOf(QLatin1Char('{'));
    if (braceStart != -1 && braceStart < arrow) {
        int braceEnd = rawPath.indexOf(QLatin1Char('}'), arrow);
        if (braceEnd != -1) {
            QString prefix = rawPath.left(braceStart);
            QString suffix = rawPath.mid(braceEnd + 1);
            QString newPart = rawPath.mid(arrow + 4, braceEnd - (arrow + 4));
            return prefix + newPart + suffix;
        }
    }
    return rawPath.mid(arrow + 4).trimmed();
}
} // namespace

QHash<QString, GitRepositoryService::GitNumstatEntry> GitRepositoryService::parseNumstat(const QByteArray &output)
{
    QHash<QString, GitNumstatEntry> result;
    const auto lines = output.split('\n');
    for (const QByteArray &line : lines) {
        if (line.isEmpty()) continue;
        const auto parts = line.split('\t');
        if (parts.size() >= 3) {
            GitNumstatEntry entry;
            if (parts[0] == "-" || parts[1] == "-") {
                entry.binary = true;
                entry.additions = 0;
                entry.deletions = 0;
            } else {
                entry.additions = parts[0].toInt();
                entry.deletions = parts[1].toInt();
            }
            const QString rawPath = decode(parts[2]).trimmed();
            const QString resolved = resolveNumstatPath(rawPath);
            const QString normalizedResolved = normalizedGitPath(resolved);
            result.insert(normalizedResolved, entry);
            if (resolved != rawPath) {
                result.insert(normalizedGitPath(rawPath), entry);
            }
        }
    }
    return result;
}

namespace {
// Rows of one file section. Header lines ("--- a/x", "+++ b/x") only occur
// before the first hunk, so inside hunks a leading "-"/"+" is always a
// change — a removed "-- comment" line is not mistaken for a header.
QSharedPointer<GitDiffBody> parseDiffBody(const QByteArray &section, bool binary) {
    auto body = QSharedPointer<GitDiffBody>::create();
    body->binary = binary;
    body->digest = qHash(section);
    int oldLine = 0, newLine = 0;
    bool inHunk = false;
    static const QRegularExpression hunk(QStringLiteral(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)"));
    const QByteArrayView all(section);
    for (qsizetype start = 0; start < all.size();) {
        qsizetype end = section.indexOf('\n', start);
        if (end < 0)
            end = all.size();
        QByteArrayView raw = all.sliced(start, end - start);
        start = end + 1;
        if (raw.endsWith('\r'))
            raw.chop(1);
        if (raw.isEmpty())
            continue;
        if (raw.startsWith("@@")) {
            const auto match = hunk.match(decode(raw.toByteArray()));
            if (match.hasMatch()) {
                oldLine = match.captured(1).toInt();
                newLine = match.captured(3).toInt();
                inHunk = true;
            }
            continue;
        }
        if (!inHunk)
            continue;
        const char lead = raw.front();
        const QString text = QString::fromUtf8(raw.mid(1));
        if (lead == '-') {
            ++body->deletions;
            body->append(GitDiffLine::Delete, oldLine++, 0, text);
        } else if (lead == '+') {
            ++body->additions;
            body->append(GitDiffLine::Add, 0, newLine++, text);
        } else if (lead == ' ') {
            body->append(GitDiffLine::Context, oldLine++, newLine++, text);
        }
    }
    return body;
}
} // namespace

QList<GitDiffFile> GitRepositoryService::parseUnifiedDiffFiles(const QByteArray &output) {
    QList<GitDiffFile> files;
    QByteArray section;
    QString oldPath;
    QString path;
    QString status = QStringLiteral("modified");
    bool binary = false;
    bool inHunk = false;

    const auto flush = [&]() {
        if (section.isEmpty()) return;
        const QString displayPath = path.isEmpty() ? oldPath : path;
        if (displayPath.isEmpty()) {
            section.clear();
            return;
        }
        QSharedPointer<GitDiffBody> body = parseDiffBody(section, binary);
        QString resolvedStatus = status;
        if (oldPath.isEmpty() && !path.isEmpty()) resolvedStatus = QStringLiteral("added");
        else if (!oldPath.isEmpty() && path.isEmpty()) resolvedStatus = QStringLiteral("deleted");
        QVariantMap item = changeItem(displayPath,
                                      oldPath != displayPath ? oldPath : QString(),
                                      resolvedStatus, false, false, false);
        item.insert(QStringLiteral("additions"), body->additions);
        item.insert(QStringLiteral("deletions"), body->deletions);
        item.insert(QStringLiteral("binary"), binary);
        files.push_back(GitDiffFile{item, body});
        section.clear();
    };
    const QList<QByteArray> rows = output.split('\n');
    for (const QByteArray &row : rows) {
        if (row.startsWith("diff --git ")) {
            flush();
            oldPath.clear();
            path.clear();
            status = QStringLiteral("modified");
            binary = false;
            inHunk = false;
            const QByteArray header = row.mid(11);
            QByteArray aBytes, bBytes;
            if (parseDiffGitHeader(header, aBytes, bBytes)) {
                oldPath = decodePatchPath(aBytes);
                path = decodePatchPath(bBytes);
            }
        }
        section += row;
        section += '\n';
        if (inHunk) continue;
        if (row.startsWith("@@")) inHunk = true;
        else if (row.startsWith("--- ")) oldPath = decodePatchPath(row.mid(4));
        else if (row.startsWith("+++ ")) path = decodePatchPath(row.mid(4));
        else if (row.startsWith("new file mode ")) status = QStringLiteral("added");
        else if (row.startsWith("deleted file mode ")) status = QStringLiteral("deleted");
        else if (row.startsWith("rename from ")) {
            oldPath = decodePatchPath(row.mid(12));
            status = QStringLiteral("renamed");
        } else if (row.startsWith("rename to ")) {
            path = decodePatchPath(row.mid(10));
            status = QStringLiteral("renamed");
        } else if (row.startsWith("copy from ")) {
            oldPath = decodePatchPath(row.mid(10));
            status = QStringLiteral("copied");
        } else if (row.startsWith("copy to ")) {
            path = decodePatchPath(row.mid(8));
            status = QStringLiteral("copied");
        } else if (row.startsWith("Binary files ") || row == QByteArrayLiteral("GIT binary patch")) {
            binary = true;
        }
    }
    flush();
    return files;
}
