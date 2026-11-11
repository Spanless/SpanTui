#pragma once

#include <QHash>
#include <QSet>
#include <QString>

class MarkdownGfmAdapter final {
public:
    struct Result {
        QString markdown;
        QSet<int> centeredBlocks;
        QHash<QString, qreal> imageWidths;
    };

    static Result adapt(const QString &source, bool dark);
};
