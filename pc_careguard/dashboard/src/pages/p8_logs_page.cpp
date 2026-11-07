#include "p8_logs_page.h"

#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QTabWidget>
#include <QVBoxLayout>

#include "net/server_config.h"
#include "net/ws_client.h"
#include "p8/p8_log_tab.h"
#include "theme/tokens.h"

namespace {

// 값이 없을 때 "-"로 보여주는 문자열 필드.
QString textOrDash(const QJsonObject& obj, const char* key) {
    return obj.value(key).toString(QStringLiteral("-"));
}

P8LogTabSpec auditSpec() {
    P8LogTabSpec spec;
    spec.endpoint = QStringLiteral("/audit-log");
    spec.searchPlaceholder = QStringLiteral("계정·행위·대상 검색");
    spec.headers = {QStringLiteral("시각"), QStringLiteral("계정"), QStringLiteral("행위"),
                    QStringLiteral("대상 · 상세")};
    spec.columnWidths = {200, 220, 260, 0};
    spec.stretchColumn = 3;
    spec.monoColumns = {0};
    spec.highlightColumn = 2;  // 영상 열람·이벤트 완료 등 책임 추적 대상 행의 "행위"
    spec.toCells = [](const QJsonObject& obj) {
        return QStringList{formatLogTime(obj.value("occurred_at").toString()), textOrDash(obj, "account"),
                           obj.value("action").toString(), obj.value("detail").toString()};
    };
    return spec;
}

P8LogTabSpec systemEventSpec() {
    P8LogTabSpec spec;
    spec.endpoint = QStringLiteral("/system-events");
    spec.searchPlaceholder = QStringLiteral("로봇·종류·메시지 검색");
    spec.headers = {QStringLiteral("시각"), QStringLiteral("로봇"), QStringLiteral("종류"),
                    QStringLiteral("메시지")};
    spec.columnWidths = {160, 100, 160, 0};
    spec.stretchColumn = 3;
    spec.monoColumns = {0};
    spec.highlightColumn = 2;  // severity == error(복귀 필요) 등
    spec.toCells = [](const QJsonObject& obj) {
        return QStringList{formatLogTime(obj.value("occurred_at").toString()), textOrDash(obj, "robot_code"),
                           obj.value("event_type").toString(), obj.value("message").toString()};
    };
    return spec;
}

P8LogTabSpec patrolRunSpec() {
    P8LogTabSpec spec;
    spec.endpoint = QStringLiteral("/patrol-runs/log");
    spec.searchPlaceholder = QStringLiteral("로봇·대상자·경로·상태 검색");
    spec.headers = {QStringLiteral("시작 시각"), QStringLiteral("로봇"), QStringLiteral("대상자"),
                    QStringLiteral("경로"),     QStringLiteral("유형"), QStringLiteral("상태"),
                    QStringLiteral("진행")};
    spec.columnWidths = {140, 70, 110, 0, 60, 60, 60};
    spec.stretchColumn = 3;
    spec.monoColumns = {0, 6};
    spec.highlightColumn = 5;  // 중단·실패한 순찰의 "상태"
    spec.toCells = [](const QJsonObject& obj) {
        return QStringList{formatLogTime(obj.value("started_at").toString()),
                           textOrDash(obj, "robot_code"),
                           textOrDash(obj, "recipient_name"),
                           textOrDash(obj, "route_name"),
                           obj.value("trigger").toString(),
                           obj.value("status").toString(),
                           QStringLiteral("%1/%2")
                               .arg(obj.value("waypoints_done").toInt())
                               .arg(obj.value("waypoints_total").toInt())};
    };
    return spec;
}

}  // namespace

P8LogsPage::P8LogsPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                               Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("통합 로그"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    const QString serverUrl = ServerConfig::resolveServerUrl();
    auto* net = new QNetworkAccessManager(this);
    auditTab_ = new P8LogTab(auditSpec(), net, serverUrl, this);
    systemEventTab_ = new P8LogTab(systemEventSpec(), net, serverUrl, this);
    patrolRunTab_ = new P8LogTab(patrolRunSpec(), net, serverUrl, this);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(auditTab_, QStringLiteral("사용자 로그"));
    tabs->addTab(systemEventTab_, QStringLiteral("임계값·알림 로그"));
    tabs->addTab(patrolRunTab_, QStringLiteral("경로순찰 로그"));
    layout->addWidget(tabs);

    auditTab_->refresh();
    systemEventTab_->refresh();
    patrolRunTab_->refresh();
}

void P8LogsPage::setWsClient(WsClient* client) {
    wsClient_ = client;
    connect(wsClient_, &WsClient::messageReceived, this, &P8LogsPage::handleWsMessage);
}

void P8LogsPage::handleWsMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    // 지금 적용 중인 기간·검색 필터를 그대로 쓴 채로 다시 불러온다. 새 항목이 지금 필터 범위 밖(예:
    // 어제 날짜로 보고 있는데 방금 생긴 오늘 이벤트)이면 화면엔 안 나타나는데, 필터가 의도한 정상 동작이다.
    if (type == QStringLiteral("audit_log_created")) {
        auditTab_->refresh();
    } else if (type == QStringLiteral("system_event_created")) {
        systemEventTab_->refresh();
    } else if (type == QStringLiteral("patrol_run_created") || type == QStringLiteral("patrol_progress") ||
               type == QStringLiteral("patrol_done")) {
        patrolRunTab_->refresh();
    }
}
