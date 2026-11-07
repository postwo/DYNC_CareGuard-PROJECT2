#pragma once
#include <QWidget>

class QJsonObject;
class QNetworkAccessManager;
class WsClient;
class P8LogTab;

// P8 통합 로그 — 사용자 로그 / 임계값·알림 로그 / 경로순찰 로그 3탭.
// 2026-09-11 사이드바 개편으로 시스템 설정(P7)에서 분리된 독립 페이지다.
//
// 세 탭은 기간·검색 필터와 표가 같고 조회하는 주소와 열만 다르다. 그래서 이 클래스는 탭 세 개를
// 설정(P8LogTabSpec)과 함께 만들고, 서버가 보내는 실시간 알림에 맞춰 해당 탭을 다시 조회시키는 일만
// 한다. 화면과 조회는 pages/p8/p8_log_tab.*가 맡는다.
class P8LogsPage : public QWidget {
    Q_OBJECT
public:
    explicit P8LogsPage(QWidget* parent = nullptr);

    // MainWindow가 앱 전체에서 공유하는 WsClient 하나를 내려준다 — 페이지마다 자기 WsClient를
    // 따로 만들면 /ws/dashboard 소켓이 페이지 수만큼 중복으로 열린다.
    void setWsClient(WsClient* client);

private:
    // 2026-09-12 추가 — 세 탭 다 원래는 페이지를 열거나 필터를 바꿀 때만 다시 불렀다(진짜 "실시간"은
    // 아니었음). /ws/dashboard의 audit_log_created/system_event_created/patrol_run_created(및
    // patrol_progress/patrol_done)를 받으면 해당 탭을 다시 불러 지금 적용 중인 필터 그대로 최신 상태로
    // 갱신한다.
    void handleWsMessage(const QJsonObject& message);

    WsClient* wsClient_ = nullptr;
    P8LogTab* auditTab_ = nullptr;
    P8LogTab* systemEventTab_ = nullptr;
    P8LogTab* patrolRunTab_ = nullptr;
};
