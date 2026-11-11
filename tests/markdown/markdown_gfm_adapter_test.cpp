#include "documents/markdown_gfm_adapter.h"

#include <QTest>

class MarkdownGfmAdapterTest final : public QObject {
    Q_OBJECT

private slots:
    void preservesStandardMarkdown();
    void neverRewritesCode();
    void convertsBoundedRawHtml();
    void normalizesGitHubReadmeExtensions();
    void preservesOpaqueMarkdownDuringNormalization();
};

void MarkdownGfmAdapterTest::preservesStandardMarkdown() {
    const QString source = QStringLiteral(
        "# Heading\n\n- [x] task\n\n| a | b |\n| - | - |\n| 1 | 2 |");
    QCOMPARE(MarkdownGfmAdapter::adapt(source, true).markdown, source);
}

void MarkdownGfmAdapterTest::neverRewritesCode() {
    const QString source = QStringLiteral(
        "```html\n<img src=\"https://example.test/fenced.png\">\n```\n\n"
        "`<br><img src='https://example.test/inline.png'>`\n\n"
        "``<br>\n<img src='https://example.test/multiline.png'>``\n\n"
        "    <h1>indented</h1>");
    QCOMPARE(MarkdownGfmAdapter::adapt(source, false).markdown, source);
}

void MarkdownGfmAdapterTest::convertsBoundedRawHtml() {
    const QString source = QStringLiteral(
        "<div align=\"center\"><h2>Title</h2>"
        "<img src=\"https://example.test/a.png\" alt=\"A\" width=\"320\">"
        "</div>");
    const MarkdownGfmAdapter::Result result = MarkdownGfmAdapter::adapt(source, true);
    QCOMPARE(result.markdown,
             QStringLiteral("## Title\n\n![A](https://example.test/a.png)"));
    QCOMPARE(result.imageWidths.value(QStringLiteral("https://example.test/a.png")),
             qreal(320));
    QCOMPARE(result.centeredBlocks, QSet<int>({0, 1}));
}

void MarkdownGfmAdapterTest::normalizesGitHubReadmeExtensions() {
    const QString source = QStringLiteral(R"md(
<div align="center">
<h1>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://example.test/logo-dark.png">
    <source media="(prefers-color-scheme: light)" srcset="https://example.test/logo-light.png">
    <img alt="Project logo" src="https://example.test/logo-light.png" width="96">
  </picture>
  <br>
  Project (<code>tool</code>)
</h1>
</div>

> [!IMPORTANT]
> Keep the generated file read-only.
)md");
    const QString dark = MarkdownGfmAdapter::adapt(source, true).markdown;
    QVERIFY(!dark.contains(QStringLiteral("<picture")));
    QVERIFY(!dark.contains(QStringLiteral("<div")));
    QVERIFY(dark.contains(QStringLiteral(
        "![Project logo](https://example.test/logo-dark.png)")));
    QVERIFY(dark.contains(QStringLiteral("# Project (`tool`)")));
    QVERIFY(dark.contains(QStringLiteral("> **Important**")));

    const QString light = MarkdownGfmAdapter::adapt(source, false).markdown;
    QVERIFY(light.contains(QStringLiteral(
        "![Project logo](https://example.test/logo-light.png)")));
}

void MarkdownGfmAdapterTest::preservesOpaqueMarkdownDuringNormalization() {
    const QString source = QStringLiteral(R"md(
```html
<img src="http://127.0.0.1/inside-fence.png">
```

Inline `<img src="http://127.0.0.1/inline-code.png">`.

<!-- <img src="http://127.0.0.1/hidden-comment.png"> -->

    <img src="http://127.0.0.1/indented-code.png">

<img alt="Visible" src="https://example.test/visible.png">
)md");
    const QString normalized = MarkdownGfmAdapter::adapt(source, true).markdown;
    QVERIFY(normalized.contains(QStringLiteral(
        "<img src=\"http://127.0.0.1/inside-fence.png\">")));
    QVERIFY(normalized.contains(QStringLiteral(
        "`<img src=\"http://127.0.0.1/inline-code.png\">`")));
    QVERIFY(normalized.contains(QStringLiteral(
        "<!-- <img src=\"http://127.0.0.1/hidden-comment.png\"> -->")));
    QVERIFY(normalized.contains(QStringLiteral(
        "    <img src=\"http://127.0.0.1/indented-code.png\">")));
    QVERIFY(normalized.contains(QStringLiteral(
        "![Visible](https://example.test/visible.png)")));
    QCOMPARE(normalized.count(QStringLiteral("![")), 1);
}

QTEST_GUILESS_MAIN(MarkdownGfmAdapterTest)
#include "markdown_gfm_adapter_test.moc"
