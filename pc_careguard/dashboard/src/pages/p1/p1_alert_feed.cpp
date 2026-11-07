#include "p1_alert_feed.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/table_cells.h"

namespace {

QString badgeTextFor(const char* level) {
    if (level == Tokens::Prop::EMERGENCY) return QStringLiteral("긴급");
    if (level == Tokens::Prop::ATTENTION) return QStringLiteral("주의");
    return QStringLiteral("정보");
}

}  // namespace

P1AlertFeed::P1AlertFeed(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("실시간 알림"), this);
    title->setProperty("role", "h3");
    layout->addWidget(title);

    list_ = new QListWidget(this);
    list_->setObjectName("alertList");
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(list_);
}

void P1AlertFeed::setAlerts(const QVector<P1AlertRow>& alerts) {
    list_->clear();
    constexpr int kRowHeight = 36;
    for (const P1AlertRow& alert : alerts) {
        auto* row = new QWidget(list_);
        row->setFixedHeight(kRowHeight);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 2, 2, 2);
        rowLayout->setSpacing(10);

        auto* time = new QLabel(alert.time, row);
        time->setProperty("role", "mono");
        time->setFixedWidth(64);
        rowLayout->addWidget(time);
        rowLayout->setAlignment(time, Qt::AlignVCenter);

        auto* badge = cells::makeBadge(alert.level, badgeTextFor(alert.level));
        badge->setProperty("size", "xs");
        rowLayout->addWidget(badge);
        rowLayout->setAlignment(badge, Qt::AlignVCenter);

        auto* text = new QLabel(alert.text, row);
        rowLayout->addWidget(text, /*stretch=*/1);
        rowLayout->setAlignment(text, Qt::AlignVCenter);

        auto* item = new QListWidgetItem(list_);
        item->setSizeHint(QSize(row->sizeHint().width(), kRowHeight));
        list_->addItem(item);
        list_->setItemWidget(item, row);
    }
}
