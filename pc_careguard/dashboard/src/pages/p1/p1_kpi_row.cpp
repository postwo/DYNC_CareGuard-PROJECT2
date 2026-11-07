#include "p1_kpi_row.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QVBoxLayout>

#include "theme/tokens.h"

P1KpiRow::P1KpiRow(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Tokens::Layout::CARD_GAP);
}

void P1KpiRow::setKpis(const QVector<P1Kpi>& kpis) {
    auto* rowLayout = qobject_cast<QHBoxLayout*>(layout());
    while (QLayoutItem* item = rowLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const P1Kpi& kpi : kpis) {
        auto* card = new QFrame(this);
        card->setObjectName("kpiCard");
        if (kpi.level != nullptr) card->setProperty("level", kpi.level);

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(Tokens::Layout::CARD_PADDING, 10, Tokens::Layout::CARD_PADDING, 10);
        cardLayout->setSpacing(4);

        auto* label = new QLabel(kpi.label, card);
        label->setProperty("role", "caption");
        cardLayout->addWidget(label);

        auto* value = new QLabel(kpi.value, card);
        value->setObjectName("kpiValue");
        QFont valueFont = value->font();
        valueFont.setPixelSize(17);
        valueFont.setWeight(QFont::DemiBold);
        value->setFont(valueFont);
        cardLayout->addWidget(value);

        rowLayout->addWidget(card);
    }
}
