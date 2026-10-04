#pragma once
#include <QDate>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <vector>

#include "net/api_client.h"
#include "theme/tokens.h"

enum class P1SortMode { Risk, RobotId, Name };

// 통합 관제 표의 한 행(로봇 한 대).
struct P1RobotRow {
    QString id;
    Tokens::Level level = Tokens::Level::Normal;
    QString address;
    QString recipient;
    int battery = -1;   // %, -1 = 알 수 없음
    int medSteps = -1;  // 0~3, -1 = 알 수 없음(오늘 복약 스케줄 없음)
    QString mission;
    // RobotControlModal(상세 모달)이 취소·충전 복귀 명령에 그대로 쓸 이벤트 코드 —
    // 이 로봇에 연결된 미해결 이벤트가 없으면 비어있다.
    QString eventCode;
    QString eventText;
};

// "실시간 알림" 피드의 한 줄.
struct P1AlertRow {
    QString time;
    const char* level = nullptr;  // Tokens::Prop::EMERGENCY / ATTENTION / INFO
    QString text;
};

// KPI 카드 한 장.
struct P1Kpi {
    QString label;
    QString value;
    const char* level = nullptr;  // nullptr = 기본 흰 배경
};

// P1 통합 관제가 보여주는 데이터와, 그 데이터로부터 화면용 행·KPI·알림을 만드는 규칙.
//
// 위젯을 전혀 모른다 — 서버에서 받은 로봇·대상자·이벤트와 실시간 상태(온라인·순찰 중·배터리)를
// 받아서 계산만 한다. 그래서 화면 없이도 "오프라인이면 배터리를 숨긴다" 같은 규칙을 검증할 수 있다.
// 예전엔 이 계산이 P1OverviewPage 안에서 위젯 갱신과 뒤섞여 있었다.
class P1OverviewModel : public QObject {
    Q_OBJECT
public:
    explicit P1OverviewModel(QObject* parent = nullptr);

    // ---- 서버에서 받은 목록 -------------------------------------------------------------
    void setRobots(const QVector<ApiClient::RobotInfo>& robots);
    void setRecipients(const QVector<ApiClient::RecipientInfo>& recipients);
    void setEvents(const QVector<ApiClient::EventInfo>& events);
    void addEvent(const ApiClient::EventInfo& event);  // WS event_created — 가장 최근으로 앞에 넣는다

    // ---- 실시간 상태 -------------------------------------------------------------------
    // "순찰/이동 중"은 nav_status(Nav2 프로세스 생존)가 아니라 실제 patrol_run 진행 상태로 판단한다 —
    // Nav2는 정지 명령 후에도 계속 떠 있어서 nav_status만으로는 "대기 중"으로 안 바뀐다.
    void setPatrollingRobotIds(const QSet<int>& robotIds);
    // 취소 성공 직후 그 로봇을 낙관적으로 "대기 중"으로 반영한다(서버의 patrol_run_updated를
    // 다시 받기 전까지 클릭 결과가 안 보이면 사용자가 헷갈린다).
    void markRobotIdle(const QString& robotCode);
    void setRobotOnline(const QString& robotCode, bool online);
    void setNavRunning(const QString& robotCode, bool running);
    // 서버(robot_ws.py::BatteryFilter)가 이미 EMA + 히스테리시스로 다듬어 내려주므로 그대로 저장만
    // 한다. battery_state는 초당 여러 번 오기 때문에 신호를 내지 않는다 — 다시 그리기는 호출부가
    // rebuildRows()를 디바운스해서 부른다.
    void setLiveBattery(const QString& robotCode, int percent);
    void rebuildRows();

    void setSortMode(P1SortMode mode);
    P1SortMode sortMode() const { return sortMode_; }

    // ---- 화면용 결과 ---------------------------------------------------------------------
    const std::vector<P1RobotRow>& rows() const { return rows_; }
    QVector<P1Kpi> kpis() const;
    QVector<P1AlertRow> alerts() const;

    // 테스트에서 "오늘"을 고정할 수 있게 한다. 기본은 실제 오늘.
    void setToday(const QDate& today) { today_ = today; }

    // WS event_created 메시지 -> EventInfo (필드 이름은 GET /events의 EventItem과 같다).
    static ApiClient::EventInfo eventFromMessage(const QJsonObject& message);

signals:
    void rowsChanged();
    void kpisChanged();
    void alertsChanged();

private:
    void sortRows();
    QDate today() const { return today_.isValid() ? today_ : QDate::currentDate(); }

    QVector<ApiClient::RobotInfo> robots_;
    QVector<ApiClient::RecipientInfo> recipients_;
    QVector<ApiClient::EventInfo> events_;
    QSet<int> patrollingRobotIds_;
    QMap<QString, int> liveBattery_;
    std::vector<P1RobotRow> rows_;
    P1SortMode sortMode_ = P1SortMode::Risk;
    QDate today_;
};
