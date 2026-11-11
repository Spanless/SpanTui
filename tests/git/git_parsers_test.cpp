#include "git/git_constants.h"
#include "git/git_repository_service.h"

#include <QTest>
#include <QVariantList>
#include <QVariantMap>

namespace {

QVariantList parseWholeHistory(const QByteArray &output)
{
    GitRepositoryService::HistoryGraph graph;
    return GitRepositoryService::parseHistoryPage(output, &graph);
}

// Graph segments of `row` whose kind is `kind`, in row order.
QVariantList segmentsOfKind(const QVariantList &rows, int row, const QString &kind)
{
    QVariantList found;
    for (const QVariant &segment : rows.at(row).toMap().value(QStringLiteral("graphSegments")).toList()) {
        if (segment.toMap().value(QStringLiteral("kind")).toString() == kind) found.append(segment);
    }
    return found;
}

QStringList segmentTargets(const QVariantList &rows, int row, const QString &kind)
{
    QStringList targets;
    for (const QVariant &segment : segmentsOfKind(rows, row, kind))
        targets.append(segment.toMap().value(QStringLiteral("target")).toString());
    return targets;
}

// Every segment of every row uses one of the eight lane colours and names
// the commit its lane leads to.
bool segmentsWellFormed(const QVariantList &rows)
{
    for (const QVariant &row : rows) {
        for (const QVariant &segment : row.toMap().value(QStringLiteral("graphSegments")).toList()) {
            const int color = segment.toMap().value(QStringLiteral("color")).toInt();
            if (color < 0 || color >= 8 || segment.toMap().value(QStringLiteral("target")).toString().isEmpty())
                return false;
        }
    }
    return true;
}

} // namespace

class GitParsersTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesPublicGitFormats();
    void parsesQuotedAndBinaryDiffHeaders();
    void boundsDiffBodies();
    void parsesNumstatSummary();
    void parsesCommitPreview();
};

void GitParsersTest::parsesPublicGitFormats()
{
    QByteArray status;
    const auto statusRecord = [&status](const QByteArray &record) {
        status += record;
        status += '\0';
    };
    statusRecord("# branch.oid 0123456789abcdef0123456789abcdef01234567");
    statusRecord("# branch.head main");
    statusRecord("# branch.upstream origin/main");
    statusRecord("# branch.ab +2 -3");
    statusRecord("1 M. N... 100644 100644 100644 abc def folder/has space.txt");
    statusRecord(QByteArray("? ") + QByteArray::fromHex("e4b8ade69687") + ".txt");
    statusRecord("u UU N... 100644 100644 100644 100644 a b c conflict.txt");
    statusRecord("! build/");
    QVariantList staged;
    QVariantList working;
    QHash<QString, QString> statuses;
    const QVariantMap state =
        GitRepositoryService::parseStatus(status, &staged, &working, &statuses);
    QCOMPARE(state.value(QStringLiteral("branch")).toString(), QStringLiteral("main"));
    QVERIFY(!state.value(QStringLiteral("unborn")).toBool());
    // A branch without a commit reports "(initial)" in place of the object id.
    QByteArray unbornStatus("# branch.oid (initial)");
    unbornStatus += '\0';
    unbornStatus += "# branch.head main";
    unbornStatus += '\0';
    QVariantList unbornStaged;
    QVariantList unbornWorking;
    QHash<QString, QString> unbornStatuses;
    QVERIFY(GitRepositoryService::parseStatus(unbornStatus, &unbornStaged, &unbornWorking,
                                              &unbornStatuses)
                .value(QStringLiteral("unborn")).toBool());
    QCOMPARE(staged.size(), 2);
    QCOMPARE(working.size(), 2);
    QCOMPARE(statuses.value(QStringLiteral("folder/has space.txt")), QStringLiteral("modified"));
    QCOMPARE(statuses.value(QString::fromUtf8(QByteArray::fromHex("e4b8ade69687") + ".txt")),
             QStringLiteral("added"));
    QCOMPARE(statuses.value(QStringLiteral("conflict.txt")), QStringLiteral("conflict"));
    QCOMPARE(statuses.value(QStringLiteral("build")), QStringLiteral("ignored"));

    QByteArray branches;
    const auto branch = [&branches](const QByteArrayList &fields) {
        branches += fields.join(QByteArray(1, '\0'));
        branches += '\n';
    };
    branch({"refs/heads/main", "main", "*", "origin/main", "[ahead 2, behind 1]",
            "2026-01-01T00:00:00+00:00", "aaaaaaaa"});
    branch({"refs/remotes/origin/topic", "origin/topic", " ", "", "",
            "2026-01-02T00:00:00+00:00", "bbbbbbbb"});
    // An annotated tag also names the commit it peels to.
    branch({"refs/tags/v1.0", "v1.0", " ", "", "", "2026-01-03T00:00:00+00:00", "cccccccc", "dddddddd"});
    const QVariantList branchItems = GitRepositoryService::parseBranches(branches);
    QCOMPARE(branchItems.size(), 3);
    QVERIFY(branchItems.first().toMap().value(QStringLiteral("current")).toBool());
    QCOMPARE(branchItems.first().toMap().value(QStringLiteral("ahead")).toInt(), 2);
    QVERIFY(branchItems.at(1).toMap().value(QStringLiteral("remote")).toBool());
    QCOMPARE(branchItems.last().toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("tag"));
    QCOMPARE(branchItems.last().toMap().value(QStringLiteral("commitId")).toString(), QStringLiteral("dddddddd"));
    QVERIFY(branchItems.first().toMap().value(QStringLiteral("commitId")).toString().isEmpty());

    QByteArray history;
    const auto field = [&history](const QByteArray &value) {
        history += value;
        history += '\0';
    };
    field("aaaaaaaa");
    field("aaaaaaa");
    field("bbbbbbbb cccccccc");
    field("Alice");
    field("a@example.com");
    field("2026-01-01T00:00:00Z");
    field("HEAD -> main, tag: v1.0");
    field("Merge work");
    history += "body\x1e";
    field("bbbbbbbb");
    field("bbbbbbb");
    field("");
    field("Bob");
    field("b@example.com");
    field("2025-12-31T00:00:00Z");
    field("");
    field("Initial");
    history += '\0';
    history += '\x1e';
    const QVariantList commits = parseWholeHistory(history);
    QCOMPARE(commits.size(), 2);
    QCOMPARE(commits.first().toMap().value(QStringLiteral("parents")).toStringList().size(), 2);
    QCOMPARE(commits.first().toMap().value(QStringLiteral("decorations")).toList().size(), 2);
    QCOMPARE(commits.last().toMap().value(QStringLiteral("subject")).toString(),
             QStringLiteral("Initial"));
    // A later page continues the graph where the previous one stopped.
    const qsizetype split = history.indexOf('\x1e') + 1;
    GitRepositoryService::HistoryGraph graph;
    QVariantList paged = GitRepositoryService::parseHistoryPage(history.left(split), &graph);
    QCOMPARE(paged.size(), 1);
    paged += GitRepositoryService::parseHistoryPage(history.mid(split), &graph);
    QCOMPARE(paged, commits);

    const QByteArray diff =
        "diff --git a/old name.txt b/new name.txt\n"
        "similarity index 80%\nrename from old name.txt\nrename to new name.txt\n"
        "--- a/old name.txt\n+++ b/new name.txt\n@@ -1 +1,2 @@\n-old\n+new\n+extra\n"
        "diff --git a/image.png b/image.png\nnew file mode 100644\n"
        "Binary files /dev/null and b/image.png differ\n";
    const QList<GitDiffFile> diffFiles = GitRepositoryService::parseUnifiedDiffFiles(diff);
    QCOMPARE(diffFiles.size(), 2);
    QCOMPARE(diffFiles.first().item.value(QStringLiteral("oldPath")).toString(),
             QStringLiteral("old name.txt"));
    QCOMPARE(diffFiles.first().item.value(QStringLiteral("status")).toString(),
             QStringLiteral("renamed"));
    QVERIFY(!diffFiles.first().item.contains(QStringLiteral("lines")));
    const QVariantList lines = diffFiles.first().body->toVariant()
                                   .value(QStringLiteral("lines")).toList();
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.first().toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("delete"));
    QCOMPARE(lines.first().toMap().value(QStringLiteral("oldText")).toString(),
             QStringLiteral("old"));
    QCOMPARE(lines.last().toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("add"));
    QVERIFY(diffFiles.last().item.value(QStringLiteral("binary")).toBool());
    QVERIFY(diffFiles.last().body->binary);
    QByteArray nameStatus = QByteArrayList{"R100", "old name.txt", "new name.txt", "M",
                                           "tracked.txt"}
                                .join(QByteArray(1, '\0'));
    nameStatus += '\0';
    const QVariantList names = GitRepositoryService::parseNameStatus(nameStatus);
    QCOMPARE(names.size(), 2);
    QCOMPARE(names.first().toMap().value(QStringLiteral("path")).toString(),
             QStringLiteral("new name.txt"));

    const auto commit = [](const QByteArray &hash, const QByteArray &parents) {
        return QByteArrayList{hash, hash.left(7), parents, "A", "a@example.com",
                              "2026-01-01T00:00:00Z", "", hash, ""}
                   .join('\0')
            + '\x1e';
    };
    const QVariantList topology = parseWholeHistory(
        commit("C", "A B") + commit("B", "A") + commit("A", ""));
    QCOMPARE(topology.size(), 3);
    QCOMPARE(topology.first().toMap().value(QStringLiteral("nodeLane")).toInt(), 0);
    QVERIFY(topology.first().toMap().value(QStringLiteral("graphSegments")).toList().size() >= 2);
    QVERIFY(segmentsWellFormed(topology));
    // Segments name the commit their lane leads to: the merge fans out to
    // both parents, and B's row carries C's first-parent line through to A.
    QCOMPARE(segmentTargets(topology, 0, QStringLiteral("out")), (QStringList{"A", "B"}));
    QCOMPARE(segmentTargets(topology, 1, QStringLiteral("in")), QStringList{"B"});
    QCOMPARE(segmentTargets(topology, 1, QStringLiteral("through")), QStringList{"A"});
    QCOMPARE(segmentTargets(topology, 2, QStringLiteral("in")), QStringList{"A"});

    // A side branch forked below the main line's next commit (M(B F), F(P),
    // B(P)): the first-parent line keeps its column and colour past the
    // join, and the side lane curves in carrying its own colour.
    const QVariantList joined = parseWholeHistory(
        commit("M", "B F") + commit("F", "P") + commit("B", "P") + commit("P", ""));
    QCOMPARE(joined.size(), 4);
    const auto joinedRow = [&](int row) { return joined.at(row).toMap(); };
    const int mainColor = joinedRow(0).value(QStringLiteral("nodeColor")).toInt();
    const int sideColor = joinedRow(1).value(QStringLiteral("nodeColor")).toInt();
    QVERIFY(mainColor != sideColor);
    QCOMPARE(joinedRow(2).value(QStringLiteral("nodeLane")).toInt(), 0);
    QCOMPARE(joinedRow(3).value(QStringLiteral("nodeLane")).toInt(), 0);
    QCOMPARE(joinedRow(3).value(QStringLiteral("nodeColor")).toInt(), mainColor);
    const QVariantList joinedOut = segmentsOfKind(joined, 2, QStringLiteral("out"));
    QCOMPARE(joinedOut.size(), 1);
    QCOMPARE(joinedOut.at(0).toMap().value(QStringLiteral("color")).toInt(), mainColor);
    const QVariantList sideJoin = segmentsOfKind(joined, 2, QStringLiteral("through"));
    QCOMPARE(sideJoin.size(), 1);
    QCOMPARE(sideJoin.at(0).toMap().value(QStringLiteral("from")).toInt(), 1);
    QCOMPARE(sideJoin.at(0).toMap().value(QStringLiteral("to")).toInt(), 0);
    QCOMPARE(sideJoin.at(0).toMap().value(QStringLiteral("color")).toInt(), sideColor);
}

void GitParsersTest::parsesQuotedAndBinaryDiffHeaders()
{
    const QByteArray quotedDiff =
        "diff --git \"a/spaced dir/old image.png\" \"b/spaced dir/new image.png\"\n"
        "similarity index 100%\n"
        "rename from spaced dir/old image.png\n"
        "rename to spaced dir/new image.png\n"
        "Binary files \"a/spaced dir/old image.png\" and \"b/spaced dir/new image.png\" differ\n"
        "diff --git \"a/quoted file.txt\" \"b/quoted file.txt\"\n"
        "index 1111111..2222222 100644\n"
        "--- \"a/quoted file.txt\"\n"
        "+++ \"b/quoted file.txt\"\n"
        "@@ -1,1 +1,1 @@\n"
        "-hello\n"
        "+world\n"
        // core.quotePath (on by default) writes non-ASCII bytes as octal
        // escapes and control characters as C escapes.
        "diff --git \"a/\\344\\270\\255\\346\\226\\207\\tx.txt\" \"b/\\344\\270\\255\\346\\226\\207\\tx.txt\"\n"
        "new file mode 100644\n"
        "--- /dev/null\n"
        "+++ \"b/\\344\\270\\255\\346\\226\\207\\tx.txt\"\n"
        "@@ -0,0 +1 @@\n"
        "+text\n";

    const QList<GitDiffFile> files = GitRepositoryService::parseUnifiedDiffFiles(quotedDiff);
    QCOMPARE(files.size(), 3);
    QCOMPARE(files.at(2).item.value(QStringLiteral("path")).toString(),
             QStringLiteral("中文\tx.txt"));
    QCOMPARE(files.at(2).item.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
    QCOMPARE(files.first().item.value(QStringLiteral("path")).toString(),
             QStringLiteral("spaced dir/new image.png"));
    QCOMPARE(files.first().item.value(QStringLiteral("oldPath")).toString(),
             QStringLiteral("spaced dir/old image.png"));
    QVERIFY(files.first().item.value(QStringLiteral("binary")).toBool());
    QCOMPARE(files.at(1).item.value(QStringLiteral("path")).toString(),
             QStringLiteral("quoted file.txt"));
    QVERIFY(!files.at(1).item.value(QStringLiteral("binary")).toBool());
    QCOMPARE(files.at(1).body->lines.size(), 2);
}

void GitParsersTest::boundsDiffBodies()
{
    // Inside a hunk, "---"/"+++" rows are removed "--"/added "++" lines, not
    // file headers.
    const QList<GitDiffFile> dashed = GitRepositoryService::parseUnifiedDiffFiles(
        "diff --git a/notes.sql b/notes.sql\n"
        "--- a/notes.sql\n+++ b/notes.sql\n@@ -1,2 +1,2 @@\n"
        "--- old comment\n+++ new comment\n keep\n");
    QCOMPARE(dashed.size(), 1);
    QCOMPARE(dashed.first().item.value(QStringLiteral("path")).toString(),
             QStringLiteral("notes.sql"));
    QVERIFY(dashed.first().item.value(QStringLiteral("oldPath")).toString().isEmpty());
    QCOMPARE(dashed.first().body->deletions, 1);
    QCOMPARE(dashed.first().body->additions, 1);
    QCOMPARE(dashed.first().body->lines.first().text, QStringLiteral("-- old comment"));

    // Per-file caps: rows past kGitDiffMaxLines are dropped while the counts still
    // cover the whole diff; overlong text is cut.
    const int extra = 10;
    QByteArray big = "diff --git a/gen.txt b/gen.txt\n--- a/gen.txt\n+++ b/gen.txt\n@@ -0,0 +1 @@\n";
    big += '+' + QByteArray(kGitDiffMaxColumns * 3, 'x') + '\n';
    big += QByteArray("+row\n").repeated(kGitDiffMaxLines + extra - 1);
    const QList<GitDiffFile> capped = GitRepositoryService::parseUnifiedDiffFiles(big);
    QCOMPARE(capped.size(), 1);
    const GitDiffBodyPtr body = capped.first().body;
    QCOMPARE(body->lines.size(), kGitDiffMaxLines);
    QCOMPARE(body->totalLines, kGitDiffMaxLines + extra);
    QCOMPARE(body->additions, kGitDiffMaxLines + extra);
    QVERIFY(body->truncated);
    QCOMPARE(body->lines.first().text.size(), kGitDiffMaxColumns + 1);
    QCOMPARE(capped.first().item.value(QStringLiteral("additions")).toInt(), kGitDiffMaxLines + extra);

    // The store keeps the most recently used bodies within its byte budget
    // and reports whether an insert changed the content.
    const auto makeBody = [](size_t digest) {
        auto made = QSharedPointer<GitDiffBody>::create();
        made->append(GitDiffLine::Add, 0, 1, QString(100, QLatin1Char('a')));
        made->digest = digest;
        return GitDiffBodyPtr(made);
    };
    const qsizetype unit = makeBody(0)->bytes;
    GitDiffStore store(unit * 2);
    QVERIFY(store.insert(QStringLiteral("a"), makeBody(1)));
    QVERIFY(store.insert(QStringLiteral("b"), makeBody(2)));
    QVERIFY(store.find(QStringLiteral("a")));
    QVERIFY(store.insert(QStringLiteral("c"), makeBody(3)));
    QVERIFY(!store.contains(QStringLiteral("b")));
    QCOMPARE(store.paths(), (QStringList{"a", "c"}));
    QCOMPARE(store.bytes(), unit * 2);
    QVERIFY(!store.insert(QStringLiteral("c"), makeBody(3)));
    QVERIFY(store.insert(QStringLiteral("c"), makeBody(4)));
    store.clear();
    QCOMPARE(store.size(), 0);
    QCOMPARE(store.bytes(), 0);
}

void GitParsersTest::parsesNumstatSummary()
{
    const QByteArray numstat =
        "10\t5\tplain.txt\n"
        "-\t-\timage.png\n"
        "20\t0\tsrc/{old.cpp => new.cpp}\n"
        "0\t15\told_dir/file.txt => new_dir/file.txt\n"
        "1\t1\t中文/文件.txt\n";

    const auto stats = GitRepositoryService::parseNumstat(numstat);

    QVERIFY(stats.contains(QStringLiteral("plain.txt")));
    QCOMPARE(stats.value(QStringLiteral("plain.txt")).additions, 10);
    QCOMPARE(stats.value(QStringLiteral("plain.txt")).deletions, 5);
    QVERIFY(!stats.value(QStringLiteral("plain.txt")).binary);

    QVERIFY(stats.contains(QStringLiteral("image.png")));
    QVERIFY(stats.value(QStringLiteral("image.png")).binary);
    QCOMPARE(stats.value(QStringLiteral("image.png")).additions, 0);
    QCOMPARE(stats.value(QStringLiteral("image.png")).deletions, 0);

    QVERIFY(stats.contains(QStringLiteral("src/new.cpp")));
    QCOMPARE(stats.value(QStringLiteral("src/new.cpp")).additions, 20);
    QCOMPARE(stats.value(QStringLiteral("src/new.cpp")).deletions, 0);

    QVERIFY(stats.contains(QStringLiteral("new_dir/file.txt")));
    QCOMPARE(stats.value(QStringLiteral("new_dir/file.txt")).additions, 0);
    QCOMPARE(stats.value(QStringLiteral("new_dir/file.txt")).deletions, 15);

    QVERIFY(stats.contains(QStringLiteral("中文/文件.txt")));
    QCOMPARE(stats.value(QStringLiteral("中文/文件.txt")).additions, 1);
    QCOMPARE(stats.value(QStringLiteral("中文/文件.txt")).deletions, 1);
}

void GitParsersTest::parsesCommitPreview()
{
    QByteArray output("feat(icons): two-tier corner radius\n"
                      "\n"
                      "Containers use r=3.\r\n"
                      "Slanted corners are exempt.\n"
                      "\n"
                      "\n"
                      "\n"
                      "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n"
                      "co-authored-by: Someone Else\n");
    output += '\0';
    output += "\n 23 files changed, 24 insertions(+), 23 deletions(-)\n";

    const QVariantMap preview = GitRepositoryService::parseCommitPreview(output);
    QCOMPARE(preview.value(QStringLiteral("body")).toString(),
             QStringLiteral("Containers use r=3.\nSlanted corners are exempt."));
    const QVariantList coAuthors = preview.value(QStringLiteral("coAuthors")).toList();
    QCOMPARE(coAuthors.size(), 2);
    QCOMPARE(coAuthors.at(0).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Claude Opus 5.5"));
    QCOMPARE(coAuthors.at(0).toMap().value(QStringLiteral("email")).toString(),
             QStringLiteral("noreply@anthropic.com"));
    QCOMPARE(coAuthors.at(1).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Someone Else"));
    QVERIFY(preview.value(QStringLiteral("hasStats")).toBool());
    QCOMPARE(preview.value(QStringLiteral("filesChanged")).toInt(), 23);
    QCOMPARE(preview.value(QStringLiteral("insertions")).toInt(), 24);
    QCOMPARE(preview.value(QStringLiteral("deletions")).toInt(), 23);

    // Subject-only message, one-sided totals.
    QByteArray single("fix: typo\n");
    single += '\0';
    single += "\n 1 file changed, 2 deletions(-)\n";
    const QVariantMap small = GitRepositoryService::parseCommitPreview(single);
    QVERIFY(small.value(QStringLiteral("body")).toString().isEmpty());
    QVERIFY(small.value(QStringLiteral("coAuthors")).toList().isEmpty());
    QCOMPARE(small.value(QStringLiteral("filesChanged")).toInt(), 1);
    QCOMPARE(small.value(QStringLiteral("insertions")).toInt(), 0);
    QCOMPARE(small.value(QStringLiteral("deletions")).toInt(), 2);

    // Empty commit: no shortstat line at all.
    QByteArray empty("chore: empty\n");
    empty += '\0';
    QVERIFY(!GitRepositoryService::parseCommitPreview(empty)
                 .value(QStringLiteral("hasStats")).toBool());
}

QTEST_MAIN(GitParsersTest)
#include "git_parsers_test.moc"
