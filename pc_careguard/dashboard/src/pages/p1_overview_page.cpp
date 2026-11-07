#include "p1_overview_page.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QVBoxLayout>

#include "dialogs/robot_control_modal.h"
#include "net/ws_client.h"
#include "p1/p1_alert_feed.h"
#include "p1/p1_kpi_row.h"
#include "p1/p1_overview_model.h"
#include "p1/p1_robot_table.h"
#include "theme/tokens.h"

namespace {

QString levelLabel(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return QStringLiteral("긴급");
        case Tokens::Level::Attention: return QStringLiteral("주의");
        case Tokens::Level::Offline:   return QStringLiteral("오프라인");
        case Tokens::Level::Normal:    return QStringLiteral("정상");
    }
    return QString();
}

}  // namespace

P1OverviewPage::P1OverviewPage(QWidget* parent) : QWidget(parent) {
    apiClient_ = new ApiClient(this);
    model_ = new P1OverviewModel(this);

    batteryRefreshTimer_ = new QTimer(this);
    batteryRefreshTimer_->setSingleShot(true);
    connect(batteryRefreshTimer_, &QTimer::timeout, model_, &P1OverviewModel::rebuildRows);

    wsHandlers_ = {
        {QStringLiteral("battery_state"), &P1OverviewPage::onBatteryState},
        {QStringLiteral("robot_status"), &P1OverviewPage::onRobotStatus},
        {QStringLiteral("nav_status"), &P1OverviewPage::onNavStatus},
        {QStringLiteral("patrol_run_created"), &P1OverviewPage::onPatrolRunChanged},
        {QStringLiteral("patrol_run_updated"), &P1OverviewPage::onPatrolRunChanged},
        {QStringLiteral("event_created"), &P1OverviewPage::onEventCreated},
        {QStringLiteral("recipient_changed"), &P1OverviewPage::onRecipientChanged},
        {QStringLiteral("robot_recipient_changed"), &P1OverviewPage::onRecipientChanged},
        {QStringLiteral("system_event_created"), &P1OverviewPage::onSystemEventCreated},
    };

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                               Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    layout->addWidget(buildHeaderRow());

    loadErrorBanner_ = new QFrame(this);
    loadErrorBanner_->setObjectName("banner");
    loadErrorBanner_->setProperty("level", Tokens::Prop::ATTENTION);
    auto* bannerLayout = new QHBoxLayout(loadErrorBanner_);
    bannerLayout->setContentsMargins(10, 9, 10, 9);
    loadErrorLabel_ = new QLabel(loadErrorBanner_);
    bannerLayout->addWidget(loadErrorLabel_);
    loadErrorBanner_->setVisible(false);
    layout->addWidget(loadErrorBanner_);

    kpiRow_ = new P1KpiRow(this);
    layout->addWidget(kpiRow_);
    robotTable_ = new P1RobotTable(this);
    layout->addWidget(robotTable_, /*stretch=*/2);
    alertFeed_ = new P1AlertFeed(this);
    layout->addWidget(alertFeed_, /*stretch=*/1);

    // 모델이 바뀌면 해당 위젯만 다시 그린다.
    connect(model_, &P1OverviewModel::rowsChanged, this, [this] { robotTable_->setRows(model_->rows()); });
    connect(model_, &P1OverviewModel::kpisChanged, this, [this] { kpiRow_->setKpis(model_->kpis()); });
    connect(model_, &P1OverviewModel::alertsChanged, this, [this] { alertFeed_->setAlerts(model_->alerts()); });

    connect(robotTable_, &P1RobotTable::sortModeChanged, model_,
            [this](P1SortMode mode) { model_->setSortMode(mode); });
    connect(robotTable_, &P1RobotTable::detailRequested, this, &P1OverviewPage::openRobotDetail);
    connect(robotTable_, &P1RobotTable::stopRequested, this, &P1OverviewPage::stopRobot);

    // 데이터가 오기 전의 빈 화면(KPI는 "0/0"처럼 보인다).
    kpiRow_->setKpis(model_->kpis());
    loadFromServer();
}

void P1OverviewPage::setWsClient(WsClient* client) {
    wsClient_ = client;
    connect(wsClient_, &WsClient::messageReceived, this, &P1OverviewPage::handleWsMessage);
    // 시작할 때 서버가 아직 안 떠 있어 일부 목록을 못 받았다면, 연결이 맺어진 시점에 다시 시도한다.
    connect(wsClient_, &WsClient::connectionStateChanged, this, [this](bool connected) {
        if (connected && !failedLoads_.isEmpty()) loadFromServer();
    });
}

QWidget* P1OverviewPage::buildHeaderRow() {
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("통합 관제"), row);
    title->setObjectName("pageTitle");
    rowLayout->addWidget(title);
    rowLayout->addStretch();
    return row;
}

// ---- 서버에서 불러오기 -------------------------------------------------------------------

void P1OverviewPage::loadFromServer() {
    // 서로 독립적으로 로드되고, 도착할 때마다 그때까지 모인 데이터로 화면을 갱신한다 — 셋 다 오기를
    // 기다렸다가 한 번에 그리면 느린 쪽(대개 이벤트) 하나 때문에 나머지까지 오래 비어 있다.
    apiClient_->fetchRobots([this](bool ok, const QVector<ApiClient::RobotInfo>& items) {
        trackLoad(QStringLiteral("로봇"), ok);
        if (ok) model_->setRobots(items);
    });
    refreshRecipients();
    refreshEvents();
    refreshPatrolRunStatus();
}

void P1OverviewPage::refreshRecipients() {
    apiClient_->fetchRecipients([this](bool ok, const QVector<ApiClient::RecipientInfo>& items) {
        trackLoad(QStringLiteral("대상자"), ok);
        if (ok) model_->setRecipients(items);
    });
}

void P1OverviewPage::refreshEvents() {
    apiClient_->fetchEvents([this](bool ok, const QVector<ApiClient::EventInfo>& items) {
        trackLoad(QStringLiteral("이벤트"), ok);
        if (ok) model_->setEvents(items);
    });
}

void P1OverviewPage::refreshPatrolRunStatus() {
    // robotId <= 0 -> robot_id 필터 없이 전체 로봇의 실행 중(status=running) 목록을 한 번에 받는다.
    apiClient_->fetchPatrolRuns(0, QStringLiteral("running"),
                                [this](bool ok, const QVector<ApiClient::PatrolRunInfo>& runs) {
        trackLoad(QStringLiteral("순찰"), ok);
        if (!ok) return;
        QSet<int> robotIds;
        for (const auto& run : runs) robotIds.insert(run.robotId);
        model_->setPatrollingRobotIds(robotIds);
    });
}

void P1OverviewPage::trackLoad(const QString& source, bool ok) {
    if (ok) failedLoads_.remove(source);
    else failedLoads_.insert(source);

    loadErrorBanner_->setVisible(!failedLoads_.isEmpty());
    if (!failedLoads_.isEmpty()) {
        QStringList names = failedLoads_.values();
        names.sort();
        loadErrorLabel_->setText(
            QStringLiteral("%1 정보를 불러오지 못했습니다 — 서버 연결을 확인하세요.").arg(names.join(QStringLiteral(", "))));
    }
}

// ---- 실시간(WS) --------------------------------------------------------------------------

void P1OverviewPage::handleWsMessage(const QJsonObject& message) {
    const WsHandler handler = wsHandlers_.value(message.value(QStringLiteral("type")).toString(), nullptr);
    if (handler != nullptr) (this->*handler)(message);
}

void P1OverviewPage::onBatteryState(const QJsonObject& message) {
    model_->setLiveBattery(message.value(QStringLiteral("robot_code")).toString(),
                           static_cast<int>(message.value(QStringLiteral("percentage")).toDouble()));
    // 값은 서버가 이미 다듬어 보내지만 다시 그리기는 계속 디바운스한다 — 여러 대가 동시에 쏟아내도
    // 최대 1초에 한 번만 그려야 호버 등이 깜빡이지 않는다.
    if (!batteryRefreshTimer_->isActive()) batteryRefreshTimer_->start(1000);
}

void P1OverviewPage::onRobotStatus(const QJsonObject& message) {
    model_->setRobotOnline(message.value(QStringLiteral("robot_code")).toString(),
                           message.value(QStringLiteral("online")).toBool());
}

void P1OverviewPage::onNavStatus(const QJsonObject& message) {
    model_->setNavRunning(message.value(QStringLiteral("robot_code")).toString(),
                          message.value(QStringLiteral("running")).toBool());
}

void P1OverviewPage::onPatrolRunChanged(const QJsonObject&) {
    // 이 창에서 시작/취소한 것이든(낙관적 갱신은 markRobotIdle이 이미 처리) 다른 대시보드 창·
    // scheduler.py 자동 트리거로 바뀐 것이든 서버가 같은 이벤트로 브로드캐스트하므로, 구분 없이
    // 실제 상태를 다시 조회해 맞춘다.
    refreshPatrolRunStatus();
}

void P1OverviewPage::onEventCreated(const QJsonObject& message) {
    // scheduler.py/robot_ws.py가 care_event를 새로 커밋한 직후 보내는 알림 — 페이지를 새로고침하지
    // 않아도 방금 생긴 이벤트가 바로 목록에 뜨게 한다.
    model_->addEvent(P1OverviewModel::eventFromMessage(message));
}

void P1OverviewPage::onRecipientChanged(const QJsonObject&) {
    // recipient.py::update_recipient(이름 등 기본정보 저장)나 robots.py::assign_robot_recipient(담당
    // 로봇 재배정)가 보내는 알림 — 대상자 목록은 시작할 때 한 번만 받아오므로 이게 없으면 이미 켜져 있던
    // 다른 창의 통합관제는 재시작 전까지 옛 값을 보여준다.
    refreshRecipients();
}

void P1OverviewPage::onSystemEventCreated(const QJsonObject&) {
    // robot_ws.py가 배터리 임계값 등 시스템 이벤트를 감지했을 때 보내는 알림 — payload 없이 type만
    // 오므로 바로 반영할 수 없어 목록을 통째로 다시 받는다.
    refreshEvents();
}

// ---- 사용자 동작 -------------------------------------------------------------------------

void P1OverviewPage::openRobotDetail(const P1RobotRow& row) {
    RobotControlModal::Data data;
    data.robotCode = row.id;
    data.headerTitle = QStringLiteral("%1  %2 · %3").arg(row.id, row.address, row.recipient);
    data.urgencyBadge = (row.level == Tokens::Level::Emergency || row.level == Tokens::Level::Attention)
                            ? levelLabel(row.level)
                            : QString();
    data.mapLabel = QStringLiteral("%1 맵").arg(row.address);
    data.batteryPct = row.battery;
    data.eventCode = row.eventCode;
    data.eventText = row.eventText;
    // 통신 지연(ms)·카메라/마이크/Nav2 개별 상태·현재 위치·최근 미션 이력은 서버에 그 자체를 재거나
    // 조회하는 경로가 없어 못 채운다 — 지어낸 값을 보여주는 것보다 비워 두는 편이 낫다.

    auto* modal = new RobotControlModal(data, wsClient_, this);
    modal->setAttribute(Qt::WA_DeleteOnClose);
    connect(modal, &RobotControlModal::eventNavigationRequested, this, &P1OverviewPage::eventNavigationRequested);
    modal->show();
}

void P1OverviewPage::stopRobot(const QString& robotCode) {
    // 로봇 개별 비상정지(모터 즉시 정지) API가 서버에 없어, 헤더의 "전체 비상정지"와 같은 이유로
    // 순찰/이동 취소(cancelPatrolRun)로 대신한다.
    apiClient_->cancelPatrolRun(robotCode, [this, robotCode](bool ok, const QString& error) {
        if (ok) {
            model_->markRobotIdle(robotCode);
            return;
        }
        QMessageBox::warning(this, QStringLiteral("정지 실패"),
                             error.isEmpty() ? QStringLiteral("%1 연결을 확인해주세요.").arg(robotCode) : error);
    });
}
