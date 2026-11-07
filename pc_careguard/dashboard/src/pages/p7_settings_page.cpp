#include "p7_settings_page.h"

#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>

#include "net/server_config.h"
#include "net/ws_client.h"
#include "p7/p7_account_tab.h"
#include "p7/p7_api.h"
#include "p7/p7_recipient_directory.h"
#include "p7/p7_robot_tab.h"
#include "p7/p7_threshold_tab.h"
#include "theme/tokens.h"

P7SettingsPage::P7SettingsPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING,
                               Tokens::Layout::PAGE_PADDING, Tokens::Layout::PAGE_PADDING);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* title = new QLabel(QStringLiteral("시스템 설정"), this);
    title->setObjectName("pageTitle");
    layout->addWidget(title);

    api_ = new P7Api(ServerConfig::resolveServerUrl(), this);
    recipients_ = new P7RecipientDirectory(api_, this);

    robotTab_ = new P7RobotTab(api_, recipients_, this);
    accountTab_ = new P7AccountTab(api_, recipients_, this);
    thresholdTab_ = new P7ThresholdTab(api_, this);

    tabs_ = new QTabWidget(this);
    tabs_->insertTab(RobotTabIndex, robotTab_, QStringLiteral("로봇 관리"));
    tabs_->insertTab(AccountTabIndex, accountTab_, QStringLiteral("계정 관리"));
    tabs_->insertTab(ThresholdTabIndex, thresholdTab_, QStringLiteral("임계값 · 알림"));
    layout->addWidget(tabs_);

    // 로봇 목록은 로봇 탭이 소유하고, 임계값 탭의 로봇 선택 콤보가 그 목록을 따라간다.
    connect(robotTab_, &P7RobotTab::robotsChanged, this, [this] { thresholdTab_->setRobots(robotTab_->robots()); });

    // 목록은 하드코딩하지 않고 서버 DB에서 불러온다.
    accountTab_->refresh();
    recipients_->refresh();
    robotTab_->refresh();
}

void P7SettingsPage::setCurrentUserLoginId(const QString& loginId) {
    accountTab_->setCurrentUserLoginId(loginId);
}

void P7SettingsPage::setCurrentUserRole(const QString& role) {
    const bool operatorRole = (role == QStringLiteral("operator"));

    // 로봇 관리·계정 관리 탭은 operator에게 숨긴다 — 임계값·알림 탭 하나만 남는다.
    tabs_->setTabVisible(RobotTabIndex, !operatorRole);
    tabs_->setTabVisible(AccountTabIndex, !operatorRole);
    if (operatorRole) tabs_->setCurrentIndex(ThresholdTabIndex);

    // 남은 임계값·알림 탭은 operator에게 조회 전용이다.
    thresholdTab_->setReadOnly(operatorRole);
}

void P7SettingsPage::setWsClient(WsClient* client) {
    wsClient_ = client;
    connect(wsClient_, &WsClient::messageReceived, robotTab_, &P7RobotTab::handleWsMessage);
    // 서버가 재시작됐다 돌아오는 경우 등, (재)연결될 때마다 로봇 목록을 다시 불러온다 — 그 사이에
    // 놓친 robot_status push가 있어도 이걸로 따라잡는다.
    connect(wsClient_, &WsClient::connectionStateChanged, this, [this](bool connected) {
        if (connected) robotTab_->refresh();
    });
}
