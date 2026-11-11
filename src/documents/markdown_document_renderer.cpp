#include "documents/markdown_document_renderer.h"

#include <QFutureWatcher>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>

namespace {
constexpr int kRebuildDelayMs = 90;

QString githubHeadingSlug(const QString &heading) {
    QString slug;
    bool pendingHyphen = false;
    for (const QChar character : heading.trimmed().toLower()) {
        if (character.isLetterOrNumber()
            || character == QLatin1Char('_')
            || character == QLatin1Char('-')) {
            if (pendingHyphen && !slug.isEmpty()
                && !slug.endsWith(QLatin1Char('-'))) {
                slug += QLatin1Char('-');
            }
            pendingHyphen = false;
            slug += character;
        } else if (character.isSpace()) {
            pendingHyphen = true;
        }
    }
    return slug;
}
}

MarkdownDocumentRenderer::MarkdownDocumentRenderer(QObject *parent)
    : QObject(parent), m_imageLoader(this), m_rebuildTimer(this) {
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(kRebuildDelayMs);
    connect(&m_rebuildTimer, &QTimer::timeout,
            this, &MarkdownDocumentRenderer::rebuild);
    connect(&m_imageLoader, &MarkdownRemoteImageLoader::progressChanged,
            this, &MarkdownDocumentRenderer::remoteImageProgressChanged);
    connect(&m_imageLoader, &MarkdownRemoteImageLoader::cancelled,
            this, &MarkdownDocumentRenderer::remoteImageCancelled);
    connect(&m_imageLoader, &MarkdownRemoteImageLoader::imageReady, this,
            [this](const QUrl &url, const QImage &image) {
                if (!m_document || image.isNull())
                    return;
                m_document->addResource(QTextDocument::ImageResource, url, image);
                m_document->markContentsDirty(0, m_document->characterCount());
                scheduleDecorations();
            });
    connect(&m_imageLoader, &MarkdownRemoteImageLoader::imageEvicted, this,
            [this](const QUrl &url) {
                if (!m_document)
                    return;
                m_document->addResource(QTextDocument::ImageResource, url, QImage{});
                m_document->markContentsDirty(0, m_document->characterCount());
            });
}

QString MarkdownDocumentRenderer::source() const { return m_source; }

bool MarkdownDocumentRenderer::active() const { return m_active; }
bool MarkdownDocumentRenderer::rendering() const { return m_rendering; }
bool MarkdownDocumentRenderer::dark() const { return m_dark; }
qreal MarkdownDocumentRenderer::maximumImageWidth() const { return m_maximumImageWidth; }
int MarkdownDocumentRenderer::pendingRemoteImageCount() const {
    return m_imageLoader.pendingCount();
}
int MarkdownDocumentRenderer::remoteImageTotal() const {
    return m_imageLoader.totalCount();
}
int MarkdownDocumentRenderer::remoteImageFailedCount() const {
    return m_imageLoader.failedCount();
}
int MarkdownDocumentRenderer::remoteImageCount() const
{
    return m_remoteImageUrls.size();
}
bool MarkdownDocumentRenderer::remoteImagesAllowed() const
{
    return m_remoteImagesAllowed;
}

void MarkdownDocumentRenderer::setSource(const QString &source) {
    if (m_source == source)
        return;
    m_source = source;
    emit sourceChanged();
    cancelRemoteImages();
    resetRemoteImages();
    ++m_renderGeneration;
    if (!m_active)
        return;
    if (m_renderedMarkdown.isEmpty())
        rebuild();
    else
        m_rebuildTimer.start();
}

void MarkdownDocumentRenderer::setActive(bool active) {
    if (m_active == active)
        return;
    m_active = active;
    emit activeChanged();
    if (!m_active) {
        m_rebuildTimer.stop();
        ++m_renderGeneration;
        cancelRemoteImages();
        if (m_rendering) {
            m_rendering = false;
            emit renderingChanged();
        }
        return;
    }
    rebuild();
}

void MarkdownDocumentRenderer::setDark(bool dark) {
    if (m_dark == dark)
        return;
    m_dark = dark;
    emit darkChanged();
    ++m_renderGeneration;
    if (m_active)
        rebuild();
}

void MarkdownDocumentRenderer::setMaximumImageWidth(qreal width) {
    const qreal normalized = std::max<qreal>(0, width);
    if (qFuzzyCompare(m_maximumImageWidth + 1, normalized + 1))
        return;
    m_maximumImageWidth = normalized;
    scheduleDecorations();
}


void MarkdownDocumentRenderer::rebuild() {
    if (!m_active)
        return;
    const QString source = m_source;
    const bool dark = m_dark;
    const quint64 generation = ++m_renderGeneration;
    if (!m_rendering) {
        m_rendering = true;
        emit renderingChanged();
    }
    emit renderingStarted();
    auto *watcher = new QFutureWatcher<MarkdownGfmAdapter::Result>(this);
    connect(watcher, &QFutureWatcher<MarkdownGfmAdapter::Result>::finished, this,
            [this, watcher, generation]() {
        const MarkdownGfmAdapter::Result normalized = watcher->result();
        watcher->deleteLater();
        if (!m_active || generation != m_renderGeneration)
            return;
        m_centeredBlocks = normalized.centeredBlocks;
        m_imageWidths = normalized.imageWidths;
        m_renderedMarkdown = normalized.markdown;
        renderAttachedDocument();
        m_rendering = false;
        emit renderingChanged();
        emit renderingFinished();
    });
    watcher->setFuture(QtConcurrent::run([source, dark]() {
        return MarkdownGfmAdapter::adapt(source, dark);
    }));
}

void MarkdownDocumentRenderer::attach(QObject *quickTextDocument) {
    auto *wrapper = qobject_cast<QQuickTextDocument *>(quickTextDocument);
    attachDocument(wrapper ? wrapper->textDocument() : nullptr);
}

void MarkdownDocumentRenderer::attachDocument(QTextDocument *document) {
    if (m_document == document)
        return;
    QObject::disconnect(m_contentsConnection);
    m_document = document;
    if (m_document) {
        m_contentsConnection =
            connect(m_document, &QTextDocument::contentsChanged,
                    this, &MarkdownDocumentRenderer::scheduleDecorations);
        if (!m_source.isEmpty())
            renderAttachedDocument();
    }
}

void MarkdownDocumentRenderer::renderAttachedDocument() {
    if (!m_document)
        return;
    m_document->setMarkdown(
        m_renderedMarkdown,
        static_cast<QTextDocument::MarkdownFeatures>(
            QTextDocument::MarkdownDialectGitHub
            | QTextDocument::MarkdownNoHTML));
    // Synchronous so renderingFinished observes the centered/sized document.
    applyDecorations();
}

void MarkdownDocumentRenderer::setDocumentFont(const QFont &font) {
    if (m_document)
        m_document->setDefaultFont(font);
}
void MarkdownDocumentRenderer::scheduleDecorations() {
    if (!m_document || m_decorationsQueued || m_applyingDecorations)
        return;
    m_decorationsQueued = true;
    QTimer::singleShot(0, this, &MarkdownDocumentRenderer::applyDecorations);
}

void MarkdownDocumentRenderer::applyDecorations() {
    m_decorationsQueued = false;
    if (!m_document || m_applyingDecorations)
        return;
    m_applyingDecorations = true;
    QTextCursor transaction(m_document);
    transaction.beginEditBlock();
    int blockIndex = 0;
    for (QTextBlock block = m_document->begin(); block.isValid();
         block = block.next(), ++blockIndex) {
        QTextBlockFormat blockFormat = block.blockFormat();
        if (m_centeredBlocks.contains(blockIndex)) {
            blockFormat.setAlignment(Qt::AlignHCenter);
            QTextCursor blockCursor(block);
            blockCursor.setBlockFormat(blockFormat);
        }
        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                continue;
            QTextImageFormat imageFormat = fragment.charFormat().toImageFormat();
            const QString name = imageFormat.name();
            const qreal requestedWidth = m_imageWidths.value(name);
            qreal targetWidth = requestedWidth;
            if (targetWidth <= 0 && m_maximumImageWidth > 0) {
                const QImage image =
                    m_document->resource(QTextDocument::ImageResource, QUrl(name))
                        .value<QImage>();
                targetWidth = image.isNull()
                                  ? m_maximumImageWidth
                                  : std::min<qreal>(image.width(), m_maximumImageWidth);
            } else if (targetWidth > 0 && m_maximumImageWidth > 0) {
                targetWidth = std::min(targetWidth, m_maximumImageWidth);
            }
            if (targetWidth > 0 && !qFuzzyCompare(imageFormat.width(), targetWidth)) {
                imageFormat.setWidth(targetWidth);
                QTextCursor imageCursor(m_document);
                imageCursor.setPosition(fragment.position());
                imageCursor.setPosition(fragment.position() + fragment.length(),
                                        QTextCursor::KeepAnchor);
                imageCursor.setCharFormat(imageFormat);
            }
        }
    }
    transaction.endEditBlock();
    m_applyingDecorations = false;
    requestRemoteImages();
}

void MarkdownDocumentRenderer::setNetworkPolicyForTesting(MarkdownRemoteImageLoader::NetworkPolicy policy)
{
    m_imageLoader.setNetworkPolicyForTesting(std::move(policy));
}

void MarkdownDocumentRenderer::allowRemoteImages()
{
    if (m_remoteImagesAllowed) return;
    m_remoteImagesAllowed = true;
    emit remoteImagesChanged();
    requestRemoteImages();
}

// Collects the document's remote image URLs on every decoration pass; only an
// explicit allowRemoteImages() hands them to the loader.
void MarkdownDocumentRenderer::requestRemoteImages() {
    if (!m_active || !m_document)
        return;
    QSet<QUrl> urls;
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                continue;
            const QUrl url(fragment.charFormat().toImageFormat().name());
            if (url.scheme() == QStringLiteral("https")
                || url.scheme() == QStringLiteral("http")) {
                urls.insert(url);
            }
        }
    }
    if (urls != m_remoteImageUrls) {
        m_remoteImageUrls = urls;
        emit remoteImagesChanged();
    }
    if (m_remoteImagesAllowed) m_imageLoader.request(urls);
}

void MarkdownDocumentRenderer::resetRemoteImages()
{
    if (!m_remoteImagesAllowed && m_remoteImageUrls.isEmpty()) return;
    m_remoteImagesAllowed = false;
    m_remoteImageUrls.clear();
    emit remoteImagesChanged();
}

void MarkdownDocumentRenderer::cancelRemoteImages() {
    m_imageLoader.cancel();
}

int MarkdownDocumentRenderer::anchorPosition(const QString &href) const {
    if (!m_document || !href.startsWith(QLatin1Char('#')) || href.size() <= 1)
        return -1;
    const QString requested =
        QUrl::fromPercentEncoding(href.mid(1).toUtf8()).toLower();
    QHash<QString, int> slugOccurrences;
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid())
                continue;
            const QTextCharFormat format = fragment.charFormat();
            if (format.anchorNames().contains(requested, Qt::CaseInsensitive))
                return fragment.position();
        }
        if (block.blockFormat().property(QTextFormat::HeadingLevel).toInt() <= 0)
            continue;
        const QString baseSlug = githubHeadingSlug(block.text());
        if (baseSlug.isEmpty())
            continue;
        const int occurrence = slugOccurrences.value(baseSlug);
        slugOccurrences.insert(baseSlug, occurrence + 1);
        const QString slug = occurrence == 0
                                 ? baseSlug
                                 : baseSlug + QLatin1Char('-') + QString::number(occurrence);
        if (slug == requested)
            return block.position();
    }
    return -1;
}
