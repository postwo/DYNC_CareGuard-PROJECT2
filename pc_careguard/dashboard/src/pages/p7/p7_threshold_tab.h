#pragma once
#include <QVector>
#include <QWidget>

#include "p7_robot_tab.h"

class QComboBox;
class QLineEdit;
class QPushButton;
class P7Api;

// P7 "임계값 · 알림" 탭 — 로봇별 임계값 조회·저장, 전체 로봇 일괄 적용.
class P7ThresholdTab : public QWidget {
    Q_OBJECT
public:
    explicit P7ThresholdTab(P7Api* api, QWidget* parent = nullptr);

    // 로봇 목록이 바뀔 때마다 부른다. 목록(id·코드)이 실제로 달라졌을 때만 콤보를 다시 채운다 —
    // 안 그러면 로봇 상태만 바뀌어도 서버 값으로 덮어써져 사용자가 입력 중이던 값이 사라진다.
    void setRobots(const QVector<P7Robot>& robots);

    // operator 역할은 조회 전용 — 입력·저장 버튼을 막고 입력칸처럼 안 보이게 한다.
    void setReadOnly(bool readOnly);

private:
    // 한 로봇의 임계값(서버 RobotThresholds 중 이 화면이 다루는 다섯 개).
    struct Values {
        int batteryAttention = 30;
        int batteryReturn = 28;
        int voiceWait = 8;
        int medMissedWait = 600;
        int medReannounce = 2;
    };

    void buildUi();
    void fetchForCurrentRobot();
    void showValues(const Values& values);
    // 입력칸을 검증해 Values로 읽는다. 잘못된 칸이 있으면 메시지를 띄우고 false.
    bool readValues(Values* out);
    QJsonObject toJson(const Values& values) const;
    void saveForCurrentRobot();
    void applyToAllRobots();

    P7Api* api_;
    QVector<P7Robot> robots_;
    QComboBox* robotCombo_ = nullptr;
    QLineEdit* batteryAttentionEdit_ = nullptr;
    QLineEdit* batteryReturnEdit_ = nullptr;
    QLineEdit* voiceWaitEdit_ = nullptr;
    QLineEdit* medMissedWaitEdit_ = nullptr;
    QLineEdit* medReannounceEdit_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* applyAllButton_ = nullptr;
};
