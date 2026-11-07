#pragma once
#include <QString>
#include <QVector>

#include "dialogs/p7_account_dialog.h"

// 계정 관리 탭의 계정 한 건.
struct P7Account {
    int dbId = -1;   // careguard_user.id
    QString id;      // login_id
    QString name;
    QString role;    // 관리자 / 관제 / 보호자 (화면 표기)
    QString charge;  // 담당 대상자·로봇 요약
    QString lastLogin;
    bool active = false;
};

// 계정 화면이 따르는 판단 규칙. 위젯을 전혀 쓰지 않아 화면 없이 검증할 수 있다.
namespace p7rules {

// loginId의 계정이 관리자인지(목록에 없으면 false — 목록을 불러오기 전엔 아무도 관리자가 아니다).
bool isAdmin(const QVector<P7Account>& accounts, const QString& loginId);

// dbId가 "활성 관리자가 이 계정 하나뿐"인 계정인지 — 마지막 관리자를 잠그거나 내리지 못하게 막는 데 쓴다.
bool isLastActiveAdmin(const QVector<P7Account>& accounts, int dbId);

// 서버 role.code <-> 화면 표기 <-> 다이얼로그 enum. role.name(예: "관제 담당자")이 아니라 표에서
// 쓰는 짧은 표기로 맞춘다.
QString roleCodeToKorean(const QString& code);
QString roleEnumToCode(AccountDialog::Role role);
AccountDialog::Role koreanRoleToEnum(const QString& role);

}  // namespace p7rules
