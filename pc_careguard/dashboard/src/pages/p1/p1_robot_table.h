#pragma once
#include <QWidget>
#include <vector>

#include "p1_overview_model.h"

class QTableWidget;

// P1의 "로봇·대상자 현황" — 정렬 버튼과 표. 데이터를 가공하지 않고 받은 행을 그리기만 한다.
class P1RobotTable : public QWidget {
    Q_OBJECT
public:
    explicit P1RobotTable(QWidget* parent = nullptr);
    void setRows(const std::vector<P1RobotRow>& rows);

signals:
    void sortModeChanged(P1SortMode mode);
    void detailRequested(const P1RobotRow& row);
    void stopRequested(const QString& robotCode);

private:
    void buildUi();
    void populateRow(int index, const P1RobotRow& row);

    QTableWidget* table_ = nullptr;
};
