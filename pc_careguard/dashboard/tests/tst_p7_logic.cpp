#include <QtTest>

#include "pages/p7/p7_account_rules.h"
#include "pages/p7/p7_api.h"

namespace {

P7Account account(int dbId, const QString& id, const QString& role, bool active) {
    P7Account a;
    a.dbId = dbId;
    a.id = id;
    a.role = role;
    a.active = active;
    return a;
}

P7Api::Result result(int status, const QByteArray& json) {
    P7Api::Result r;
    r.status = status;
    if (!json.isEmpty()) {
        r.json = QJsonDocument::fromJson(json);
        r.jsonValid = !r.json.isNull();
    }
    return r;
}

}  // namespace

class P7LogicTest : public QObject {
    Q_OBJECT

private slots:
    // ---- 계정 판단 규칙 --------------------------------------------------------------
    void isAdminOnlyForListedAdmin() {
        const QVector<P7Account> accounts = {account(1, "admin", "관리자", true), account(2, "op", "관제", true)};
        QVERIFY(p7rules::isAdmin(accounts, "admin"));
        QVERIFY(!p7rules::isAdmin(accounts, "op"));
        QVERIFY(!p7rules::isAdmin(accounts, "nobody"));
        QVERIFY(!p7rules::isAdmin({}, "admin"));  // 목록을 불러오기 전엔 아무도 관리자가 아니다
    }

    void lastActiveAdminIsProtected() {
        QVector<P7Account> accounts = {account(1, "a1", "관리자", true), account(2, "op", "관제", true)};
        QVERIFY(p7rules::isLastActiveAdmin(accounts, 1));
        QVERIFY(!p7rules::isLastActiveAdmin(accounts, 2));  // 관리자가 아니다

        accounts.push_back(account(3, "a2", "관리자", true));
        QVERIFY(!p7rules::isLastActiveAdmin(accounts, 1));  // 다른 활성 관리자가 있다

        accounts[2].active = false;                          // 그 관리자가 비활성이면 다시 마지막
        QVERIFY(p7rules::isLastActiveAdmin(accounts, 1));
        QVERIFY(!p7rules::isLastActiveAdmin(accounts, 3));  // 비활성 계정은 해당 없음
    }

    void roleMappingRoundTrips() {
        for (const QString code : {"admin", "operator", "guardian"}) {
            const QString korean = p7rules::roleCodeToKorean(code);
            QCOMPARE(p7rules::roleEnumToCode(p7rules::koreanRoleToEnum(korean)), code);
        }
        QCOMPARE(p7rules::roleCodeToKorean("something-new"), QString("관제"));  // 모르는 코드는 가장 낮은 권한 표기
    }

    // ---- 서버 오류 메시지 --------------------------------------------------------------
    void serverDetailIsShownVerbatim() {
        QCOMPARE(result(400, R"({"detail":"비밀번호는 8자 이상이어야 합니다"})").errorMessage(),
                 QString("비밀번호는 8자 이상이어야 합니다"));
    }

    void validationErrorArrayBecomesReadableMessage() {
        const auto r = result(422, R"({"detail":[{"loc":["body","code"],"msg":"too short","type":"x"}]})");
        QCOMPARE(r.errorMessage(), QString("입력값이 올바르지 않습니다"));
    }

    void networkFailureAndUnknownHttpErrorsAreDistinguished() {
        QCOMPARE(result(0, "").errorMessage(), QString("서버에 연결할 수 없습니다"));
        QVERIFY(result(500, "<html>boom</html>").errorMessage().contains("HTTP 500"));
        QVERIFY(result(502, "").errorMessage().contains("502"));
    }

    void statusHelpers() {
        const auto r = result(201, "");
        QVERIFY(r.is(201));
        QVERIFY(r.isOneOf(200, 201));
        QVERIFY(!r.is(200));
    }
};

QTEST_MAIN(P7LogicTest)
#include "tst_p7_logic.moc"
