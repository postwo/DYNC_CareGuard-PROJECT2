#include <QtTest>

#include "pages/p1/p1_overview_model.h"

namespace {

ApiClient::RobotInfo makeRobot(int id, const QString& code, bool online) {
    ApiClient::RobotInfo r;
    r.id = id;
    r.code = code;
    r.isOnline = online;
    return r;
}

ApiClient::RecipientInfo makeRecipient(const QString& name, const QString& robotCode, const QString& level = "normal") {
    ApiClient::RecipientInfo r;
    r.name = name;
    r.robotCode = robotCode;
    r.homeAddress = name + "의 집";
    r.level = level;
    return r;
}

ApiClient::EventInfo makeEvent(const QString& uid, const QString& robotCode, const QString& severity, bool resolved,
                           const QDateTime& at) {
    ApiClient::EventInfo e;
    e.eventUid = uid;
    e.robotCode = robotCode;
    e.severity = severity;
    e.resolved = resolved;
    e.occurredAt = at;
    e.eventTypeLabel = "라벨";
    return e;
}

}  // namespace

class P1OverviewModelTest : public QObject {
    Q_OBJECT

private slots:
    void offlineRobotShowsNoBatteryAndOfflineLevel() {
        P1OverviewModel model;
        model.setRobots({makeRobot(1, "R-01", true)});
        model.setRecipients({makeRecipient("김순자", "R-01", "emergency")});
        model.setLiveBattery("R-01", 77);
        model.rebuildRows();
        QCOMPARE(model.rows().front().battery, 77);
        QVERIFY(model.rows().front().level == Tokens::Level::Emergency);

        model.setRobotOnline("R-01", false);
        // 통신이 끊기면 마지막 배터리 값과 긴급 배지를 그대로 두면 "지금도 유효"하다는 오해를 준다
        QCOMPARE(model.rows().front().battery, -1);
        QVERIFY(model.rows().front().level == Tokens::Level::Offline);
        QCOMPARE(model.rows().front().mission, QString("통신 두절"));
    }

    void missionFollowsRealPatrolRunNotNavStatus() {
        P1OverviewModel model;
        model.setRobots({makeRobot(7, "R-07", true)});
        QCOMPARE(model.rows().front().mission, QString("대기 중"));

        model.setNavRunning("R-07", true);  // Nav2가 떠 있어도 순찰 중은 아니다
        QCOMPARE(model.rows().front().mission, QString("대기 중"));

        model.setPatrollingRobotIds({7});
        QCOMPARE(model.rows().front().mission, QString("순찰/이동 중"));

        model.markRobotIdle("R-07");
        QCOMPARE(model.rows().front().mission, QString("대기 중"));
    }

    void rowsAreSortedByRiskThenByChosenMode() {
        P1OverviewModel model;
        model.setRobots({makeRobot(1, "R-01", true), makeRobot(2, "R-02", true), makeRobot(3, "R-03", false)});
        model.setRecipients({makeRecipient("나", "R-01", "normal"), makeRecipient("가", "R-02", "emergency")});

        // 위험순: 긴급 > 주의 > 오프라인 > 정상 — 통신이 끊긴 로봇은 정상 로봇보다 위에 둔다
        QCOMPARE(model.rows()[0].id, QString("R-02"));
        QCOMPARE(model.rows()[1].id, QString("R-03"));
        QCOMPARE(model.rows()[2].id, QString("R-01"));

        model.setSortMode(P1SortMode::RobotId);
        QCOMPARE(model.rows()[0].id, QString("R-01"));

        model.setSortMode(P1SortMode::Name);
        QCOMPARE(model.rows()[0].recipient, QString("가"));
    }

    void kpisCountOnlineOfflineAndUnresolvedEmergencies() {
        P1OverviewModel model;
        model.setRobots({makeRobot(1, "R-01", true), makeRobot(2, "R-02", false)});
        const QDateTime now = QDateTime::currentDateTime();
        model.setEvents({makeEvent("a", "R-01", "emergency", false, now), makeEvent("b", "R-01", "emergency", true, now),
                         makeEvent("c", "R-01", "attention", false, now)});

        const auto kpis = model.kpis();
        QCOMPARE(kpis[0].value, QString("1/2"));
        QCOMPARE(kpis[1].value, QString("1"));  // 해결된 것과 attention은 제외
        QVERIFY(kpis[1].level == Tokens::Prop::EMERGENCY);
        QCOMPARE(kpis[3].value, QString("1"));
    }

    void alertsKeepOnlyTodayNewestFirstAndCapped() {
        P1OverviewModel model;
        model.setToday(QDate(2026, 9, 18));
        QVector<ApiClient::EventInfo> events;
        events.push_back(makeEvent("old", "R-01", "info", false, QDateTime(QDate(2026, 9, 17), QTime(23, 0))));
        events.push_back(makeEvent("early", "R-01", "info", false, QDateTime(QDate(2026, 9, 18), QTime(1, 0))));
        events.push_back(makeEvent("late", "R-01", "emergency", false, QDateTime(QDate(2026, 9, 18), QTime(9, 0))));
        model.setEvents(events);

        const auto alerts = model.alerts();
        QCOMPARE(alerts.size(), 2);  // 어제 것은 "실시간 알림"에 안 나온다
        QVERIFY(alerts[0].level == Tokens::Prop::EMERGENCY);  // 최신순
        QCOMPARE(alerts[0].time, QString("09:00:00"));

        QVector<ApiClient::EventInfo> many;
        for (int i = 0; i < 30; ++i) many.push_back(makeEvent(QString::number(i), "R-01", "info", false, QDateTime(QDate(2026, 9, 18), QTime(10, i))));
        model.setEvents(many);
        QCOMPARE(model.alerts().size(), 20);
    }

    void latestUnresolvedEventIsAttachedToItsRobotRow() {
        P1OverviewModel model;
        model.setRobots({makeRobot(1, "R-01", true)});
        const QDateTime base = QDateTime(QDate(2026, 9, 18), QTime(8, 0));
        model.setEvents({makeEvent("older", "R-01", "attention", false, base),
                         makeEvent("newer", "R-01", "emergency", false, base.addSecs(60)),
                         makeEvent("done", "R-01", "emergency", true, base.addSecs(120))});
        QCOMPARE(model.rows().front().eventCode, QString("newer"));
    }

    void batteryWithoutRobotCodeIsIgnored() {
        P1OverviewModel model;
        model.setRobots({makeRobot(1, "R-01", true)});
        model.setLiveBattery("", 50);
        model.rebuildRows();
        QCOMPARE(model.rows().front().battery, -1);
    }

    void medicationStepsAreHiddenWhenNoScheduleToday() {
        P1OverviewModel model;
        model.setRobots({makeRobot(1, "R-01", true)});
        auto r = makeRecipient("김순자", "R-01");
        model.setRecipients({r});
        QCOMPARE(model.rows().front().medSteps, -1);

        r.medsToday = {{1, "done"}, {2, "pending"}, {3, "done"}};
        model.setRecipients({r});
        QCOMPARE(model.rows().front().medSteps, 2);
    }

    void eventFromMessageParsesMicrosecondTimestamps() {
        const QJsonObject message{{"event_uid", "EV-1"}, {"severity", "emergency"}, {"event_type", "INTRUDER"},
                                  {"event_type_label", "침입자"}, {"summary", "요약"},
                                  {"occurred_at", "2026-09-18T09:15:30.123456+09:00"}, {"resolved", false},
                                  {"robot_code", "R-01"}};
        const auto e = P1OverviewModel::eventFromMessage(message);
        QCOMPARE(e.eventUid, QString("EV-1"));
        QVERIFY(e.occurredAt.isValid());
        QCOMPARE(e.robotCode, QString("R-01"));
        QVERIFY(!e.resolved);
    }

    void eventFromMessageWithBadTimestampDoesNotCrashAndIsFilteredFromFeed() {
        P1OverviewModel model;
        model.setToday(QDate(2026, 9, 18));
        model.addEvent(P1OverviewModel::eventFromMessage({{"event_uid", "x"}, {"occurred_at", "garbage"}}));
        QCOMPARE(model.alerts().size(), 0);
    }
};

QTEST_MAIN(P1OverviewModelTest)
#include "tst_p1_overview_model.moc"
