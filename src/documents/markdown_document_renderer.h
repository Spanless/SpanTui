#pragma once

#include "documents/markdown_gfm_adapter.h"
#include "documents/markdown_remote_image_loader.h"

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QSet>
#include <QTimer>

class QTextDocument;

// Owns the derived, read-only presentation of a Markdown source buffer.
// DocumentManager remains the single owner of editable text; this class only
// normalizes GitHub-flavoured presentation extensions and decorates the
// QTextDocument created by the preview surface.
class MarkdownDocumentRenderer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged FINAL)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(bool rendering READ rendering NOTIFY renderingChanged FINAL)
    Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY darkChanged FINAL)
    Q_PROPERTY(qreal maximumImageWidth READ maximumImageWidth WRITE setMaximumImageWidth FINAL)
    Q_PROPERTY(int pendingRemoteImageCount READ pendingRemoteImageCount NOTIFY remoteImageProgressChanged FINAL)
    Q_PROPERTY(int remoteImageTotal READ remoteImageTotal NOTIFY remoteImageProgressChanged FINAL)
    Q_PROPERTY(int remoteImageFailedCount READ remoteImageFailedCount NOTIFY remoteImageProgressChanged FINAL)
    // Distinct http(s) image URLs in the current document. They stay unfetched
    // until allowRemoteImages(): a fetch exposes the user's address to the
    // image host, so loading is a per-document opt-in.
    Q_PROPERTY(int remoteImageCount READ remoteImageCount NOTIFY remoteImagesChanged FINAL)
    Q_PROPERTY(bool remoteImagesAllowed READ remoteImagesAllowed NOTIFY remoteImagesChanged FINAL)

public:
    explicit MarkdownDocumentRenderer(QObject *parent = nullptr);

    QString source() const;
    void setSource(const QString &source);
    bool active() const;
    void setActive(bool active);
    bool rendering() const;
    bool dark() const;
    void setDark(bool dark);
    qreal maximumImageWidth() const;
    void setMaximumImageWidth(qreal width);
    int pendingRemoteImageCount() const;
    int remoteImageTotal() const;
    int remoteImageFailedCount() const;
    int remoteImageCount() const;
    bool remoteImagesAllowed() const;

    Q_INVOKABLE void attach(QObject *quickTextDocument);
    Q_INVOKABLE void setDocumentFont(const QFont &font);
    Q_INVOKABLE int anchorPosition(const QString &href) const;
    // Lets the current source fetch its remote images. Not persisted: any
    // source change (another document, a reopen, a reload) withdraws it.
    Q_INVOKABLE void allowRemoteImages();

    void attachDocument(QTextDocument *document);
    // Tests serve images from a loopback fixture, which the product policy
    // refuses by design.
    void setNetworkPolicyForTesting(MarkdownRemoteImageLoader::NetworkPolicy policy);

signals:
    void sourceChanged();
    void activeChanged();
    void renderingChanged();
    void renderingStarted();
    void renderingFinished();
    void darkChanged();
    void remoteImageProgressChanged();
    void remoteImageCancelled();
    void remoteImagesChanged();

private:
    void rebuild();
    void renderAttachedDocument();
    void scheduleDecorations();
    void applyDecorations();
    void requestRemoteImages();
    void resetRemoteImages();
    void cancelRemoteImages();

    QString m_source;
    QString m_renderedMarkdown;
    bool m_active = true;
    bool m_rendering = false;
    quint64 m_renderGeneration = 0;
    bool m_dark = true;
    qreal m_maximumImageWidth = 0;
    QSet<int> m_centeredBlocks;
    QHash<QString, qreal> m_imageWidths;
    QPointer<QTextDocument> m_document;
    MarkdownRemoteImageLoader m_imageLoader;
    QSet<QUrl> m_remoteImageUrls;
    bool m_remoteImagesAllowed = false;
    QTimer m_rebuildTimer;
    bool m_decorationsQueued = false;
    bool m_applyingDecorations = false;
    QMetaObject::Connection m_contentsConnection;
};
