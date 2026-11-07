#pragma once
#include <QHash>
#include <QSet>
#include <QString>
#include <QWidget>

#include "net/api_client.h"

class QFrame;
class QJsonObject;
class QLabel;
class QTimer;
class WsClient;
class P1AlertFeed;
class P1KpiRow;
class P1OverviewModel;
class P1RobotTable;
struct P1RobotRow;

// P1 통합 관제 — 페이지 헤더 / KPI 요약 / 로봇·대상자 현황 테이블 / 실시간 알림 피드.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 0), §7 참고.
// 2026-09-12까지는 이 페이지 전체가 하드코딩된 목데이터였다 — GET /robots, GET /recipients,
// GET /events와 /ws/dashboard(battery_state/robot_status/nav_status)로 실제 서버 데이터에 연동했다.
// 통신 지연(ms)은 서버에 그 자체를 재는 로직이 없어 여전히 표시할 수 없고, 웨어러블 관련 KPI는 이
// 프로젝트에 웨어러블 데이터 자체가 없어 카드를 아예 뺐다.
//
// 이 클래스는 서버 호출과 WS 메시지를 받아 모델에 넣고 화면 조각을 조립하는 일만 한다.
//   P1OverviewModel  데이터 → 표 행·KPI·알림 계산(위젯을 모름, pages/p1/)
//   P1KpiRow / P1RobotTable / P1AlertFeed  계산된 결과를 그리는 위젯
class P1OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit P1OverviewPage(QWidget* parent = nullptr);

    // MainWindow가 앱 전체에서 공유하는 WsClient 하나를 내려준다 — 페이지마다 자기 WsClient를
    // 따로 만들면 /ws/dashboard 소켓이 페이지 수만큼 중복으로 열린다.
    void setWsClient(WsClient* client);

signals:
    // 로봇 상세 모달의 "이벤트로 이동" → MainWindow가 P2로 전환하고 그 이벤트를 연다
    // (P4VideoPage::eventDetailRequested와 같은 패턴).
    void eventNavigationRequested(const QString& eventCode);

private:
    using WsHandler = void (P1OverviewPage::*)(const QJsonObject&);

    QWidget* buildHeaderRow();

    void loadFromServer();
    void refreshRecipients();
    void refreshEvents();
    void refreshPatrolRunStatus();
    // 불러오기 성공/실패를 기록해 배너에 반영한다. 실패한 채로 두면 화면이 비어 있는 이유를
    // 사용자가 알 수 없다 — 예전엔 실패한 응답을 조용히 버렸다.
    void trackLoad(const QString& source, bool ok);

    void handleWsMessage(const QJsonObject& message);
    void onBatteryState(const QJsonObject& message);
    void onRobotStatus(const QJsonObject& message);
    void onNavStatus(const QJsonObject& message);
    void onPatrolRunChanged(const QJsonObject& message);
    void onEventCreated(const QJsonObject& message);
    void onRecipientChanged(const QJsonObject& message);
    void onSystemEventCreated(const QJsonObject& message);

    void openRobotDetail(const P1RobotRow& row);
    void stopRobot(const QString& robotCode);

    ApiClient* apiClient_ = nullptr;
    WsClient* wsClient_ = nullptr;
    P1OverviewModel* model_ = nullptr;
    P1KpiRow* kpiRow_ = nullptr;
    P1RobotTable* robotTable_ = nullptr;
    P1AlertFeed* alertFeed_ = nullptr;
    QFrame* loadErrorBanner_ = nullptr;
    QLabel* loadErrorLabel_ = nullptr;
    QSet<QString> failedLoads_;
    QHash<QString, WsHandler> wsHandlers_;

    // battery_state는 로봇에서 초당 여러 번 들어온다(uplink_node가 /battery_state를 받는 대로 올림,
    // 쓰로틀 없음). 올 때마다 표를 다시 그리면 셀 위젯이 매번 파괴·재생성돼 배터리 막대가 떨린다 —
    // 그래서 값만 즉시 모델에 넣고 실제 다시 그리기는 이 타이머로 묶어 최대 초당 한 번만 한다.
    QTimer* batteryRefreshTimer_ = nullptr;
};
