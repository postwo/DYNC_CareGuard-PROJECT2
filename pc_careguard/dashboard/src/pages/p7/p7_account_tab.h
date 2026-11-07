#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>

#include "dialogs/p7_account_dialog.h"
#include "p7_account_rules.h"

class QLabel;
class QPushButton;
class QTableWidget;
class P7Api;
class P7RecipientDirectory;

// P7 "계정 관리" 탭 — 계정 목록, 등록·수정·비밀번호 변경·활성화·삭제, 담당 대상자 배정.
class P7AccountTab : public QWidget {
    Q_OBJECT
public:
    P7AccountTab(P7Api* api, P7RecipientDirectory* recipients, QWidget* parent = nullptr);

    void refresh();  // GET /accounts
    // 로그인한 계정의 ID. 관리자 여부 판단(등록·삭제 노출)의 기준이 된다.
    void setCurrentUserLoginId(const QString& loginId);

private:
    void buildUi();
    void populateRow(int row);
    void refreshTable();
    int indexOfDbId(int dbId) const;
    bool currentUserIsAdmin() const;

    // 모달·확인창이 열려 있는 동안에도 목록이 갱신될 수 있어 행 번호가 아니라 careguard_user.id를
    // 붙잡는다(P7RobotTab과 같은 이유).
    void openAccountDialog(int dbId);  // dbId < 0이면 신규 등록
    void confirmActivate(int dbId);
    void handleDeleteClicked();

    void registerAccount(const AccountDialog::AccountData& data);
    void updateAccount(int dbId, const AccountDialog::AccountData& data);
    void changePassword(int dbId, const QString& newPassword);
    void activateAccount(int dbId);
    void deleteAccounts(const QVector<int>& userIds);
    void assignRecipients(int userId, const QVector<int>& recipientIds);
    void showFailure(const QString& title, const QString& message);

    P7Api* api_;
    P7RecipientDirectory* recipients_;
    QTableWidget* table_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QPushButton* registerButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QString currentUserLoginId_ = QStringLiteral("admin");  // 실제 ID가 들어오기 전 기본값
    QVector<P7Account> accounts_;
};
