#include "application/clipboard.h"

#include <QClipboard>
#include <QGuiApplication>

namespace span::application {

void Clipboard::copyText(const QString &text) const
{
    if (text.isEmpty()) {
        return;
    }
    if (auto *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
    }
}

} // namespace span::application
