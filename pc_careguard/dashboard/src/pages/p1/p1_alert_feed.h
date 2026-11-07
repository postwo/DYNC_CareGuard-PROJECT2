#pragma once
#include <QVector>
#include <QWidget>

#include "p1_overview_model.h"

class QListWidget;

// P1 하단의 "실시간 알림" 피드.
class P1AlertFeed : public QWidget {
    Q_OBJECT
public:
    explicit P1AlertFeed(QWidget* parent = nullptr);
    void setAlerts(const QVector<P1AlertRow>& alerts);

private:
    QListWidget* list_ = nullptr;
};
