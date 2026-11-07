#include "p7_account_rules.h"

namespace p7rules {

bool isAdmin(const QVector<P7Account>& accounts, const QString& loginId) {
    for (const P7Account& account : accounts) {
        if (account.id == loginId) return account.role == QStringLiteral("관리자");
    }
    return false;
}

bool isLastActiveAdmin(const QVector<P7Account>& accounts, int dbId) {
    const QString adminRole = QStringLiteral("관리자");
    bool targetIsActiveAdmin = false;
    int activeAdmins = 0;
    for (const P7Account& account : accounts) {
        if (account.role != adminRole || !account.active) continue;
        ++activeAdmins;
        if (account.dbId == dbId) targetIsActiveAdmin = true;
    }
    return targetIsActiveAdmin && activeAdmins <= 1;
}

QString roleCodeToKorean(const QString& code) {
    if (code == QStringLiteral("admin")) return QStringLiteral("관리자");
    if (code == QStringLiteral("guardian")) return QStringLiteral("보호자");
    return QStringLiteral("관제");
}

QString roleEnumToCode(AccountDialog::Role role) {
    switch (role) {
        case AccountDialog::Role::Admin: return QStringLiteral("admin");
        case AccountDialog::Role::Guardian: return QStringLiteral("guardian");
        case AccountDialog::Role::Operator: return QStringLiteral("operator");
    }
    return QString();
}

AccountDialog::Role koreanRoleToEnum(const QString& role) {
    if (role == QStringLiteral("관리자")) return AccountDialog::Role::Admin;
    if (role == QStringLiteral("보호자")) return AccountDialog::Role::Guardian;
    return AccountDialog::Role::Operator;
}

}  // namespace p7rules
