#include "p7_common.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace p7ui {

QFrame* card(QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setObjectName("card");
    return frame;
}

QLabel* sectionLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setProperty("role", "h3");
    return label;
}

QWidget* buildListHeader(const QString& title, const QString& actionText) {
    auto* row = new QWidget();
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);

    auto* label = new QLabel(title, row);
    label->setProperty("role", "label");
    rowLayout->addWidget(label);
    rowLayout->addStretch();

    if (!actionText.isEmpty()) {
        auto* btn = new QPushButton(actionText, row);
        btn->setProperty("variant", "primary");
        rowLayout->addWidget(btn);
    }
    return row;
}

}  // namespace p7ui
