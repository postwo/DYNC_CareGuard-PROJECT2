#pragma once
#include <QVector>
#include <QWidget>

#include "p1_overview_model.h"

// P1 상단의 KPI 카드 줄. 카드는 데이터가 (재)로드될 때마다 통째로 다시 만든다.
class P1KpiRow : public QWidget {
    Q_OBJECT
public:
    explicit P1KpiRow(QWidget* parent = nullptr);
    void setKpis(const QVector<P1Kpi>& kpis);
};
