#include "p7_robot_tab.h"

#include <QAbstractItemView>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QJsonArray>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtDebug>
#include <algorithm>

#include "p7_api.h"
#include "p7_common.h"
#include "p7_recipient_directory.h"
#include "widgets/table_cells.h"

namespace {

QJsonValue nullableString(const QString& value) {
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

}  // namespace

P7RobotTab::P7RobotTab(P7Api* api, P7RecipientDirectory* recipients, QWidget* parent)
    : QWidget(parent), api_(api), recipients_(recipients) {
    buildUi();
}

void P7RobotTab::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto* headerRow = p7ui::buildListHeader(QStringLiteral("등록 로봇 %1").arg(robots_.size()),
                                            QStringLiteral("로봇 등록"));
    countLabel_ = headerRow->findChild<QLabel*>();
    if (auto* registerButton = headerRow->findChild<QPushButton*>()) {
        connect(registerButton, &QPushButton::clicked, this, [this] { openRobotDialog(-1); });
    }
    layout->addWidget(headerRow);

    table_ = new QTableWidget(this);
    const QStringList headers = {QStringLiteral("로봇"),  QStringLiteral("IP"), QStringLiteral("도메인"),
                                 QStringLiteral("맵 · 대상자"), QStringLiteral("상태"),
                                 QStringLiteral("액션")};
    table_->setColumnCount(headers.size());
    table_->setHorizontalHeaderLabels(headers);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H_COMPACT);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setShowGrid(false);
    table_->setWordWrap(false);

    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Stretch);  // 맵 · 대상자 — 남는 폭 흡수
    table_->setColumnWidth(0, 110);
    table_->setColumnWidth(1, 175);
    table_->setColumnWidth(2, 160);
    table_->setColumnWidth(4, 130);
    table_->setColumnWidth(5, 110);
    layout->addWidget(table_, /*stretch=*/1);
}

void P7RobotTab::populateRow(int r) {
    const P7Robot& row = robots_[r];
    const bool offline = !row.isOnline();
    const QColor rowBg = offline ? Tokens::BG_SUNKEN : Tokens::BG_SURFACE;
    const QColor textColor = offline ? Tokens::TEXT_MUTED : Tokens::TEXT_PRIMARY;
    const QColor subTextColor = offline ? Tokens::TEXT_MUTED : Tokens::TEXT_SECONDARY;

    auto addItem = [&](int column, const QString& text, const QColor& color, bool mono) {
        auto* item = new QTableWidgetItem(text);
        if (mono) item->setFont(QFont(Tokens::Font::MONO));
        item->setTextAlignment(Qt::AlignCenter);
        item->setBackground(rowBg);
        item->setForeground(color);
        table_->setItem(r, column, item);
    };
    addItem(0, row.id, textColor, true);
    addItem(1, row.ip, subTextColor, true);
    addItem(2, row.ns, subTextColor, true);
    addItem(3, row.mapRecipient, textColor, false);
    addItem(4, row.statusDetail, offline ? Tokens::TEXT_MUTED : Tokens::OK.main, false);

    // 오프라인 로봇도 수정·등록 해제 다이얼로그를 열 수 있어야 한다 — 예전엔 이 칸이 클릭 안 되는
    // "재연결" 라벨이라 오프라인 로봇은 등록 해제할 방법이 아예 없었다.
    auto* editButton = new QPushButton(offline ? QStringLiteral("수정 · 삭제") : QStringLiteral("수정 · 재시작"));
    editButton->setObjectName("linkBtn");
    editButton->setCursor(Qt::PointingHandCursor);
    const int dbId = row.dbId;
    connect(editButton, &QPushButton::clicked, this, [this, dbId] { openRobotDialog(dbId); });
    table_->setCellWidget(r, 5, cells::wrapCellWidget(editButton, rowBg));
}

void P7RobotTab::refreshTable() {
    countLabel_->setText(QStringLiteral("등록 로봇 %1").arg(robots_.size()));
    table_->setRowCount(robots_.size());
    for (int r = 0; r < robots_.size(); ++r) populateRow(r);
}

int P7RobotTab::indexOfDbId(int dbId) const {
    for (int i = 0; i < robots_.size(); ++i) {
        if (robots_[i].dbId == dbId) return i;
    }
    return -1;
}

void P7RobotTab::showFailure(const QString& title, const QString& message) {
    QMessageBox::warning(this, title, message);
}

void P7RobotTab::refresh() {
    api_->get(QStringLiteral("/robots"), this, [this](const P7Api::Result& result) {
        if (!result.is(200)) {
            robots_.clear();
            refreshTable();
            emit robotsChanged();
            return;
        }
        if (!result.json.isArray()) {
            qWarning() << "GET /robots 응답이 배열이 아닙니다 — 기존 목록을 유지합니다";
            return;
        }

        QVector<P7Robot> rows;
        for (const QJsonValue& value : result.json.array()) {
            const QJsonObject obj = value.toObject();
            P7Robot row;
            row.dbId = obj.value("id").toInt(-1);
            row.id = obj.value("code").toString();
            row.name = obj.value("name").toString();
            row.model = obj.value("model").toString();
            row.rosDomainId = obj.value("ros_domain_id").toInt(0);
            row.ip = obj.value("ip").toString(QStringLiteral("-"));
            row.ns = row.rosDomainId > 0 ? QString::number(row.rosDomainId) : QStringLiteral("-");
            row.mapRecipient = obj.value("map_recipient").toString(QStringLiteral("-"));
            const bool online = obj.value("is_online").toBool();
            row.level = online ? Tokens::Level::Normal : Tokens::Level::Offline;
            row.statusDetail = online ? QStringLiteral("● 온라인") : QStringLiteral("오프라인");
            rows.push_back(row);
        }
        robots_ = rows;
        refreshTable();
        emit robotsChanged();
    });
}

void P7RobotTab::handleWsMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();

    // 이 창에서 저장했든 다른 대시보드 창에서 저장했든 서버가 똑같이 브로드캐스트하므로, 구분 없이
    // 다시 불러온다. robot_updated는 담당 대상자 재배정까지 덮지만, 등록 해제·새 맵 등록은 나머지
    // 이벤트로만 온다(로봇 관리 탭의 "맵/대상자" 열이 이 값을 보여준다 — robots.py::LIST_QUERY).
    if (type == QStringLiteral("robot_updated") || type == QStringLiteral("robot_recipient_changed") ||
        type == QStringLiteral("robot_deregistered") || type == QStringLiteral("map_changed")) {
        refresh();
        return;
    }

    if (type != QStringLiteral("robot_status")) return;

    const QString code = message.value(QStringLiteral("robot_code")).toString();
    const bool online = message.value(QStringLiteral("online")).toBool();
    for (int r = 0; r < robots_.size(); ++r) {
        if (robots_[r].id != code) continue;
        robots_[r].level = online ? Tokens::Level::Normal : Tokens::Level::Offline;
        robots_[r].statusDetail = online ? QStringLiteral("● 온라인") : QStringLiteral("오프라인");
        populateRow(r);  // 온라인 여부는 임계값 탭에 영향이 없어 robotsChanged()는 보내지 않는다
        return;
    }
}

void P7RobotTab::openRobotDialog(int dbId) {
    const bool isNew = (dbId < 0);
    RobotDialog::RobotData data;
    if (isNew) {
        int maxNumber = 0;
        for (const P7Robot& robot : robots_) maxNumber = std::max(maxNumber, robot.id.mid(2).toInt());
        data.code = QStringLiteral("R-%1").arg(maxNumber + 1, 2, 10, QLatin1Char('0'));
    } else {
        const int index = indexOfDbId(dbId);
        if (index < 0) return;  // 버튼을 누르기 직전에 다른 창에서 등록 해제된 경우
        const P7Robot& robot = robots_[index];
        data.code = robot.id;
        data.name = robot.name;
        data.model = robot.model;
        data.rosDomainId = robot.rosDomainId;
        data.ip = robot.ip == QStringLiteral("-") ? QString() : robot.ip;
        data.recipientLabel = robot.recipientLabel;
        data.connected = robot.isOnline();
        data.wsHost = robot.wsHost;
        data.connectionDetail = robot.connectionDetail;
    }

    QVector<RobotDialog::RecipientOption> options;
    for (const P7Recipient& recipient : recipients_->all()) {
        RobotDialog::RecipientOption option;
        option.id = recipient.id;
        option.label = recipient.label;
        options.push_back(option);
    }

    auto* dialog = new RobotDialog(isNew ? RobotDialog::Mode::Register : RobotDialog::Mode::Edit, data, options, this);
    connect(dialog, &RobotDialog::robotSaved, this, [this, dbId, isNew](const RobotDialog::RobotData& saved) {
        if (isNew) registerRobot(saved);
        else updateRobot(dbId, saved);
    });
    connect(dialog, &RobotDialog::robotDeregisterRequested, this, [this, dbId](const QString&) {
        if (dbId >= 0) deregister(dbId);
    });
    dialog->exec();
    dialog->deleteLater();
}

void P7RobotTab::registerRobot(const RobotDialog::RobotData& data) {
    const QJsonObject body{
        {"code", data.code},
        {"name", data.name},
        {"model", data.model},
        {"ros_domain_id", data.rosDomainId},
        {"ip", nullableString(data.ip)},
    };
    api_->post(QStringLiteral("/robots"), body, this, [this, recipientId = data.recipientId](const P7Api::Result& result) {
        if (!result.isOneOf(200, 201)) {
            showFailure(QStringLiteral("로봇 등록 실패"), result.errorMessage());
            return;
        }
        const int newRobotId = result.json.object().value("id").toInt(-1);
        if (recipientId > 0 && newRobotId > 0) {
            // 등록은 이미 성공했으니, 담당 대상자 배정은 별도 API로 이어서 저장한다(실패해도 목록은 새로고침).
            assignRecipient(newRobotId, recipientId);
            return;
        }
        refresh();
    });
}

void P7RobotTab::updateRobot(int robotDbId, const RobotDialog::RobotData& data) {
    const QJsonObject body{
        {"name", data.name},
        {"model", data.model},
        {"ros_domain_id", data.rosDomainId},
        {"ip", nullableString(data.ip)},
    };
    api_->put(QStringLiteral("/robots/%1").arg(robotDbId), body, this,
              [this, robotDbId, recipientId = data.recipientId](const P7Api::Result& result) {
                  if (!result.is(200)) {
                      showFailure(QStringLiteral("로봇 수정 실패"), result.errorMessage());
                      return;
                  }
                  // 담당 대상자는 별도 API라 이어서 저장한다. recipientId <= 0(미배정으로 바뀐 경우)도
                  // 호출해야 기존 배정이 풀린다.
                  assignRecipient(robotDbId, recipientId);
              });
}

void P7RobotTab::assignRecipient(int robotDbId, int recipientId) {
    // recipientId <= 0("미배정")은 서버 스키마(recipient_id: Optional[int])에 맞춰 JSON null로
    // 보내야 배정 해제로 처리된다 — 숫자를 그대로 보내면 없는 대상자 id로 오인해 404가 난다.
    const QJsonObject body{{"recipient_id", recipientId > 0 ? QJsonValue(recipientId) : QJsonValue(QJsonValue::Null)}};
    api_->put(QStringLiteral("/robots/%1/recipient").arg(robotDbId), body, this, [this](const P7Api::Result& result) {
        if (!result.is(204)) showFailure(QStringLiteral("담당 대상자 배정 실패"), result.errorMessage());
        refresh();
    });
}

void P7RobotTab::deregister(int robotDbId) {
    api_->del(QStringLiteral("/robots/%1").arg(robotDbId), this, [this](const P7Api::Result& result) {
        if (!result.is(204)) {
            showFailure(QStringLiteral("로봇 등록 해제 실패"), result.errorMessage());
            return;
        }
        refresh();
    });
}
