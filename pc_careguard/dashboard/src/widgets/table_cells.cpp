#include "widgets/table_cells.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>

namespace cells {

QWidget* wrapCellWidget(QWidget* inner, const QColor& rowBg) {
    auto* wrap = new QWidget();
    wrap->setAutoFillBackground(true);
    QPalette pal = wrap->palette();
    pal.setColor(QPalette::Window, rowBg);
    wrap->setPalette(pal);

    auto* layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(inner);
    return wrap;
}

QLabel* makeBadge(const char* level, const QString& text) {
    auto* badge = new QLabel(text);
    badge->setObjectName("badge");
    badge->setProperty("level", level);
    badge->setAlignment(Qt::AlignCenter);
    return badge;
}

QLabel* mutedDash() {
    auto* label = new QLabel(Tokens::Format::EMPTY);
    label->setProperty("role", "muted");
    label->setAlignment(Qt::AlignCenter);
    return label;
}

}  // namespace cells
