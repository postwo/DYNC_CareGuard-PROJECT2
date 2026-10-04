#include "p7_account_tab.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtDebug>
#include <algorithm>

#include "p7_api.h"
#include "p7_common.h"
#include "p7_recipient_directory.h"
#include "theme/tokens.h"
#include "widgets/table_cells.h"

namespace {

constexpr int kSelectColumn = 7;

QString formatServerTimestamp(const QString& iso) {
    if (iso.isEmpty()) return QString();
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!dt.isValid()) return QString();
    return dt.toString(QStringLiteral("MM.dd HH:mm"));
}

QJsonArray toJsonArray(const QVector<int>& ids) {
    QJsonArray array;
    for (int id : ids) array.append(id);
    return array;
}

// 확인/취소 한글 버튼이 달린 확인 대화상자. 확인을 눌렀으면 true.
bool confirm(QWidget* parent, const QString& title, const QString& text) {
    QMessageBox box(parent);
    box.setWindowTitle(title);
    box.setText(text);
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("확인"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("취소"));
    return box.exec() == QMessageBox::Ok;
}

}  // namespace

// ---- 구성 ----------------------------------------------------------------------------

P7AccountTab::P7AccountTab(P7Api* api, P7RecipientDirectory* recipients, QWidget* parent)
    : QWidget(parent), api_(api), recipients_(recipients) {
    buildUi();
}

void P7AccountTab::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto* headerRow = p7ui::buildListHeader(QStringLiteral("계정 0 · 활성 0"), QStringLiteral("계정 등록"));
    countLabel_ = headerRow->findChild<QLabel*>();
    registerButton_ = headerRow->findChild<QPushButton*>();
    // 등록·삭제는 관리자 권한 계정만 — 계정 목록을 불러온 뒤 refreshTable()이 판단해 갱신한다.
    // 그 전까지는 기본으로 막아 둔다.
    registerButton_->setEnabled(false);
    connect(registerButton_, &QPushButton::clicked, this, [this] { openAccountDialog(-1); });

    deleteButton_ = new QPushButton(QStringLiteral("삭제"), headerRow);
    deleteButton_->setProperty("variant", "secondary");
    deleteButton_->setVisible(false);
    connect(deleteButton_, &QPushButton::clicked, this, &P7AccountTab::handleDeleteClicked);
    headerRow->layout()->addWidget(deleteButton_);
    layout->addWidget(headerRow);

    table_ = new QTableWidget(this);
    const QStringList headers = {QStringLiteral("ID"),   QStringLiteral("이름"),
                                 QStringLiteral("역할"), QStringLiteral("담당 (대상자·로봇)"),
                                 QStringLiteral("최종 로그인"), QStringLiteral("상태"),
                                 QStringLiteral("액션"), QStringLiteral("선택")};
    table_->setColumnCount(headers.size());
    table_->setHorizontalHeaderLabels(headers);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setShowGrid(false);

    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->setColumnWidth(0, 110);
    table_->setColumnWidth(1, 100);
    table_->setColumnWidth(2, 100);
    table_->setColumnWidth(4, 130);
    table_->setColumnWidth(5, 90);
    table_->setColumnWidth(6, 150);
    table_->setColumnWidth(kSelectColumn, 60);
    table_->setColumnHidden(kSelectColumn, true);  // 관리자가 아니면 refreshTable()에서도 숨김 유지
    layout->addWidget(table_, /*stretch=*/1);
}

int P7AccountTab::indexOfDbId(int dbId) const {
    for (int i = 0; i < accounts_.size(); ++i) {
        if (accounts_[i].dbId == dbId) return i;
    }
    return -1;
}

bool P7AccountTab::currentUserIsAdmin() const {
    return p7rules::isAdmin(accounts_, currentUserLoginId_);
}

void P7AccountTab::showFailure(const QString& title, const QString& message) {
    QMessageBox::warning(this, title, message);
}

void P7AccountTab::setCurrentUserLoginId(const QString& loginId) {
    currentUserLoginId_ = loginId;
    refreshTable();
}

void P7AccountTab::populateRow(int r) {
    const P7Account& account = accounts_[r];
    const QColor rowBg = account.active ? Tokens::BG_SURFACE : Tokens::BG_SUNKEN;
    const QColor textColor = account.active ? Tokens::TEXT_PRIMARY : Tokens::TEXT_MUTED;
    const bool isAdminRole = account.role == QStringLiteral("관리자");

    auto addItem = [&](int column, const QString& text, const QColor& color, bool mono) {
        auto* item = new QTableWidgetItem(text);
        if (mono) item->setFont(QFont(Tokens::Font::MONO));
        item->setTextAlignment(Qt::AlignCenter);
        item->setBackground(rowBg);
        item->setForeground(color);
        table_->setItem(r, column, item);
        return item;
    };
    addItem(0, account.id, textColor, true);
    addItem(1, account.name, textColor, false);
    auto* roleItem = addItem(2, account.role, isAdminRole ? Tokens::ACCENT_HOVER : textColor, false);
    QFont roleFont = roleItem->font();
    roleFont.setWeight(isAdminRole ? QFont::DemiBold : QFont::Normal);
    roleItem->setFont(roleFont);
    addItem(3, account.charge, textColor, false);
    addItem(4, account.lastLogin, account.active ? Tokens::TEXT_SECONDARY : Tokens::TEXT_MUTED, true);

    table_->setCellWidget(
        r, 5,
        cells::wrapCellWidget(cells::makeBadge(account.active ? Tokens::Prop::OK : Tokens::Prop::OFFLINE,
                                             account.active ? QStringLiteral("활성") : QStringLiteral("비활성")),
                             rowBg));

    const int dbId = account.dbId;
    if (account.active) {
        const bool canEdit = currentUserIsAdmin();
        auto* editButton = new QPushButton(QStringLiteral("수정 · 비밀번호 변경"));
        editButton->setObjectName("linkBtn");
        editButton->setEnabled(canEdit);
        editButton->setCursor(canEdit ? Qt::PointingHandCursor : Qt::ArrowCursor);
        connect(editButton, &QPushButton::clicked, this, [this, dbId] { openAccountDialog(dbId); });
        table_->setCellWidget(r, 6, cells::wrapCellWidget(editButton, rowBg));
    } else {
        auto* activateButton = new QPushButton(QStringLiteral("활성화"));
        activateButton->setObjectName("linkBtn");
        activateButton->setCursor(Qt::PointingHandCursor);
        connect(activateButton, &QPushButton::clicked, this, [this, dbId] { confirmActivate(dbId); });
        table_->setCellWidget(r, 6, cells::wrapCellWidget(activateButton, rowBg));
    }

    // 삭제용 체크박스 — 관리자가 아니면 컬럼 자체가 숨겨져 있어 보이지 않는다.
    table_->setCellWidget(r, kSelectColumn, cells::wrapCellWidget(new QCheckBox(), rowBg));
}

void P7AccountTab::refreshTable() {
    const int activeCount = static_cast<int>(
        std::count_if(accounts_.begin(), accounts_.end(), [](const P7Account& a) { return a.active; }));
    countLabel_->setText(QStringLiteral("계정 %1 · 활성 %2").arg(accounts_.size()).arg(activeCount));

    table_->setRowCount(accounts_.size());
    for (int r = 0; r < accounts_.size(); ++r) populateRow(r);

    // 등록·삭제는 관리자 권한 계정만. 로그인 세션 연동 전이라 로그인 ID로 본인 계정을 찾아 판단한다.
    const bool admin = currentUserIsAdmin();
    registerButton_->setEnabled(admin);
    table_->setColumnHidden(kSelectColumn, !admin);
    deleteButton_->setVisible(admin);
}

void P7AccountTab::refresh() {
    api_->get(QStringLiteral("/accounts"), this, [this](const P7Api::Result& result) {
        if (!result.is(200)) {
            // 서버 연결 실패 시 목록을 비운 채로 둔다 — P4 영상 목록과 같은 방침.
            accounts_.clear();
            refreshTable();
            return;
        }
        if (!result.json.isArray()) {
            qWarning() << "GET /accounts 응답이 배열이 아닙니다 — 기존 목록을 유지합니다";
            return;
        }

        QVector<P7Account> rows;
        for (const QJsonValue& value : result.json.array()) {
            const QJsonObject obj = value.toObject();
            P7Account row;
            row.dbId = obj.value("id").toInt();
            row.id = obj.value("login_id").toString();
            row.name = obj.value("display_name").toString();
            row.role = p7rules::roleCodeToKorean(obj.value("role_code").toString());
            const QString chargeSummary = obj.value("charge_summary").toString();
            row.charge = chargeSummary.isEmpty() ? QStringLiteral("미배정") : chargeSummary;
            row.lastLogin = formatServerTimestamp(obj.value("last_login_at").toString());
            row.active = obj.value("is_active").toBool();
            rows.push_back(row);
        }
        accounts_ = rows;
        refreshTable();
    });
}

// ---- 사용자 동작 -----------------------------------------------------------------------

void P7AccountTab::openAccountDialog(int dbId) {
    const bool isNew = (dbId < 0);
    const int index = isNew ? -1 : indexOfDbId(dbId);
    if (!isNew && index < 0) return;  // 버튼을 누르기 직전에 다른 창에서 삭제된 경우

    QStringList existingIds;
    for (const P7Account& account : accounts_) existingIds << account.id;

    // 담당 계정 id로 표시 이름을 찾는다 — "현재 담당" 표기·재배정 확인 메시지 조립용.
    auto operatorName = [this](int operatorId) {
        const int i = indexOfDbId(operatorId);
        return i < 0 ? QString() : accounts_[i].name;
    };

    // 신규 등록은 아직 dbId가 없으니 전부 미체크로 보여준다(공백으로 두면 미배정).
    QVector<AccountDialog::RecipientOption> options;
    for (const P7Recipient& recipient : recipients_->all()) {
        AccountDialog::RecipientOption option;
        option.id = recipient.id;
        option.code = recipient.code;
        option.name = recipient.name;
        option.label = recipient.label;
        option.assigned = !isNew && recipient.operatorId == dbId;
        option.currentOperatorId = recipient.operatorId;
        option.currentOperatorName = recipient.operatorId == -1 ? QString() : operatorName(recipient.operatorId);
        options.push_back(option);
    }

    AccountDialog::AccountData data;
    if (!isNew) {
        const P7Account& account = accounts_[index];
        data.dbId = account.dbId;
        data.loginId = account.id;
        data.displayName = account.name;
        data.role = p7rules::koreanRoleToEnum(account.role);
        data.active = account.active;
        data.lastLogin = account.lastLogin;
        data.isSelf = (account.id == currentUserLoginId_);
        data.isLastActiveAdmin = p7rules::isLastActiveAdmin(accounts_, dbId);
    }

    auto* dialog = new AccountDialog(isNew ? AccountDialog::Mode::Register : AccountDialog::Mode::Edit, data,
                                     existingIds, options, this);
    connect(dialog, &AccountDialog::accountSaved, this, [this, dbId, isNew](const AccountDialog::AccountData& saved) {
        if (isNew) registerAccount(saved);
        else updateAccount(dbId, saved);
    });
    connect(dialog, &AccountDialog::passwordChangeRequested, this,
            [this, dbId](const QString&, const QString& newPassword) { changePassword(dbId, newPassword); });
    dialog->exec();
    dialog->deleteLater();
}

void P7AccountTab::confirmActivate(int dbId) {
    const int index = indexOfDbId(dbId);
    if (index < 0) return;
    const P7Account& target = accounts_[index];
    if (confirm(this, QStringLiteral("계정 활성화"),
                QStringLiteral("%1(%2) 계정을 다시 활성화하시겠습니까?").arg(target.name, target.id))) {
        activateAccount(dbId);
    }
}

void P7AccountTab::handleDeleteClicked() {
    QVector<int> ids;
    QStringList labels;
    for (int r = 0; r < table_->rowCount() && r < accounts_.size(); ++r) {
        QWidget* wrap = table_->cellWidget(r, kSelectColumn);
        auto* checkbox = wrap != nullptr ? wrap->findChild<QCheckBox*>() : nullptr;
        if (checkbox == nullptr || !checkbox->isChecked()) continue;
        const P7Account& account = accounts_[r];
        ids.append(account.dbId);
        labels.append(QStringLiteral("%1(%2)").arg(account.name, account.id));
    }
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("계정 삭제"), QStringLiteral("삭제할 계정을 먼저 선택하세요"));
        return;
    }
    if (!confirm(this, QStringLiteral("계정 삭제"),
                 QStringLiteral("%1 계정을 삭제하시겠습니까?\n이 작업은 되돌릴 수 없습니다.").arg(labels.join(QStringLiteral(", "))))) {
        return;
    }
    deleteAccounts(ids);
}

// ---- 서버 호출 -------------------------------------------------------------------------

void P7AccountTab::registerAccount(const AccountDialog::AccountData& data) {
    const QJsonObject body{
        {"login_id", data.loginId},
        {"password", data.password},
        {"display_name", data.displayName},
        {"role_code", p7rules::roleEnumToCode(data.role)},
        {"is_active", data.active},
    };
    api_->post(QStringLiteral("/accounts"), body, this,
               [this, assigned = data.assignedRecipientIds](const P7Api::Result& result) {
                   if (!result.isOneOf(200, 201)) {
                       showFailure(QStringLiteral("계정 등록 실패"), result.errorMessage());
                       return;
                   }
                   if (assigned.isEmpty()) {
                       refresh();  // 등록 결과를 DB 기준으로 다시 불러와 반영한다.
                       return;
                   }
                   // 등록과 동시에 담당 대상자를 골랐으면 이어서 배정한다(성공 시 목록까지 갱신).
                   assignRecipients(result.json.object().value("id").toInt(), assigned);
               });
}

void P7AccountTab::updateAccount(int dbId, const AccountDialog::AccountData& data) {
    const QJsonObject body{
        {"display_name", data.displayName},
        {"role_code", p7rules::roleEnumToCode(data.role)},
        {"is_active", data.active},
    };
    api_->patch(QStringLiteral("/accounts/%1").arg(dbId), body, this,
                [this, dbId, isActive = data.active, assigned = data.assignedRecipientIds](const P7Api::Result& result) {
                    if (!result.is(200)) {
                        showFailure(QStringLiteral("계정 수정 실패"), result.errorMessage());
                        return;
                    }
                    if (!isActive) {
                        // 비활성화하면 서버가 이 계정의 담당 배정을 전부 해제한다(update_account 참고).
                        // 여기서 다시 PUT을 보내면 방금 해제된 배정을 비활성 계정에 되살리게 된다.
                        refresh();
                        recipients_->refresh();
                        return;
                    }
                    assignRecipients(dbId, assigned);  // 성공 시 목록까지 다시 불러온다
                });
}

void P7AccountTab::changePassword(int dbId, const QString& newPassword) {
    api_->patch(QStringLiteral("/accounts/%1/password").arg(dbId), QJsonObject{{"new_password", newPassword}}, this,
                [this](const P7Api::Result& result) {
                    if (!result.is(204)) {
                        // 정책 위반(8자·영문+숫자) 같은 서버의 구체적인 사유를 그대로 보여준다.
                        showFailure(QStringLiteral("비밀번호 변경 실패"), result.errorMessage());
                        return;
                    }
                    QMessageBox::information(this, QStringLiteral("비밀번호 변경"), QStringLiteral("비밀번호가 변경되었습니다."));
                });
}

void P7AccountTab::activateAccount(int dbId) {
    const int index = indexOfDbId(dbId);
    if (index < 0) return;
    const P7Account& account = accounts_[index];
    const QJsonObject body{
        {"display_name", account.name},
        {"role_code", p7rules::roleEnumToCode(p7rules::koreanRoleToEnum(account.role))},
        {"is_active", true},
    };
    api_->patch(QStringLiteral("/accounts/%1").arg(dbId), body, this, [this](const P7Api::Result& result) {
        if (!result.is(200)) {
            showFailure(QStringLiteral("계정 활성화 실패"), result.errorMessage());
            return;
        }
        refresh();
    });
}

void P7AccountTab::deleteAccounts(const QVector<int>& userIds) {
    api_->post(QStringLiteral("/accounts/delete"), QJsonObject{{"user_ids", toJsonArray(userIds)}}, this,
               [this](const P7Api::Result& result) {
                   if (!result.is(204)) {
                       showFailure(QStringLiteral("계정 삭제 실패"), result.errorMessage());
                       return;
                   }
                   refresh();
               });
}

void P7AccountTab::assignRecipients(int userId, const QVector<int>& recipientIds) {
    api_->put(QStringLiteral("/accounts/%1/recipients").arg(userId), QJsonObject{{"recipient_ids", toJsonArray(recipientIds)}},
              this, [this](const P7Api::Result& result) {
                  if (!result.is(204)) {
                      showFailure(QStringLiteral("담당 배정 실패"),
                                  QStringLiteral("계정 정보는 저장됐지만 담당 대상자 배정에는 실패했습니다\n(%1)")
                                      .arg(result.errorMessage()));
                      return;
                  }
                  refresh();
                  recipients_->refresh();
              });
}
