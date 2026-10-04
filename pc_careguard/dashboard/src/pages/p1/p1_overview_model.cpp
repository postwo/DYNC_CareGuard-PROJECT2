#include "p1_overview_model.h"

#include <QDateTime>
#include <QRegularExpression>
#include <algorithm>

namespace {

// ApiClient::parseEvent()가 하는 것과 같은 ISO 파싱(서버 datetime.isoformat()의 마이크로초(6자리)를
// QDateTime이 기대하는 밀리초(3자리)로 자름) — 그 함수는 api_client.cpp 안에 private이라 못 쓴다.
QDateTime parseIsoDateTime(const QString& raw) {
    if (raw.isEmpty()) return QDateTime();
    QString trimmed = raw;
    static const QRegularExpression fraction(QStringLiteral("\\.(\\d{3})\\d*"));
    trimmed.replace(fraction, QStringLiteral(".\\1"));
    QDateTime dt = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (!dt.isValid()) dt = QDateTime::fromString(trimmed, Qt::ISODate);
    return dt;
}

// server/app/schemas.py::RecipientItem.level 대응. "offline"은 여기 없다(로봇 온라인 여부는 별도로 합성).
Tokens::Level riskLevelFromCode(const QString& code) {
    if (code == QStringLiteral("emergency")) return Tokens::Level::Emergency;
    if (code == QStringLiteral("attention")) return Tokens::Level::Attention;
    return Tokens::Level::Normal;
}

// 복약 3단계(투약 준비 → 개봉 → 복용 확인) 중 완료한 수. 오늘 복약 스케줄이 없으면 -1.
int medicationSteps(const ApiClient::RecipientInfo& recipient) {
    int done = 0;
    bool hasSchedule = false;
    for (const auto& med : recipient.medsToday) {
        if (med.slotNo < 1 || med.slotNo > 3) continue;
        hasSchedule = true;
        if (med.state == QStringLiteral("done")) ++done;
    }
    return hasSchedule ? done : -1;
}

}  // namespace

P1OverviewModel::P1OverviewModel(QObject* parent) : QObject(parent) {}

void P1OverviewModel::setRobots(const QVector<ApiClient::RobotInfo>& robots) {
    robots_ = robots;
    rebuildRows();
    emit kpisChanged();
}

void P1OverviewModel::setRecipients(const QVector<ApiClient::RecipientInfo>& recipients) {
    recipients_ = recipients;
    rebuildRows();
    emit kpisChanged();
}

void P1OverviewModel::setEvents(const QVector<ApiClient::EventInfo>& events) {
    events_ = events;
    rebuildRows();
    emit kpisChanged();
    emit alertsChanged();
}

void P1OverviewModel::addEvent(const ApiClient::EventInfo& event) {
    events_.push_front(event);
    rebuildRows();
    emit kpisChanged();
    emit alertsChanged();
}

void P1OverviewModel::setPatrollingRobotIds(const QSet<int>& robotIds) {
    patrollingRobotIds_ = robotIds;
    rebuildRows();
}

void P1OverviewModel::markRobotIdle(const QString& robotCode) {
    for (const auto& robot : robots_) {
        if (robot.code == robotCode) {
            patrollingRobotIds_.remove(robot.id);
            break;
        }
    }
    rebuildRows();
}

void P1OverviewModel::setRobotOnline(const QString& robotCode, bool online) {
    for (auto& robot : robots_) {
        if (robot.code == robotCode) {
            robot.isOnline = online;
            break;
        }
    }
    rebuildRows();
    emit kpisChanged();
}

void P1OverviewModel::setNavRunning(const QString& robotCode, bool running) {
    for (auto& robot : robots_) {
        if (robot.code == robotCode) {
            robot.isNavRunning = running;
            break;
        }
    }
    rebuildRows();
}

void P1OverviewModel::setLiveBattery(const QString& robotCode, int percent) {
    if (robotCode.isEmpty()) return;  // 로봇을 특정할 수 없는 메시지는 저장할 곳이 없다
    liveBattery_[robotCode] = percent;
}

void P1OverviewModel::setSortMode(P1SortMode mode) {
    sortMode_ = mode;
    sortRows();
    emit rowsChanged();
}

void P1OverviewModel::rebuildRows() {
    // 로봇 코드 -> 대상자 1건. 이 시스템은 로봇 한 대에 대상자 한 명이 배정되는 구조라 1:1로 찾는다.
    QMap<QString, const ApiClient::RecipientInfo*> recipientByRobot;
    for (const auto& recipient : recipients_) {
        if (!recipient.robotCode.isEmpty()) recipientByRobot[recipient.robotCode] = &recipient;
    }

    // 로봇 코드 -> 가장 최근 미해결 이벤트("이벤트로 이동" 버튼에 쓸 이벤트 코드).
    QMap<QString, const ApiClient::EventInfo*> latestUnresolvedByRobot;
    for (const auto& event : events_) {
        if (event.resolved || event.robotCode.isEmpty()) continue;
        auto it = latestUnresolvedByRobot.find(event.robotCode);
        if (it == latestUnresolvedByRobot.end() || event.occurredAt > (*it)->occurredAt) {
            latestUnresolvedByRobot[event.robotCode] = &event;
        }
    }

    rows_.clear();
    for (const auto& robot : robots_) {
        P1RobotRow row;
        row.id = robot.code;

        const ApiClient::RecipientInfo* recipient = recipientByRobot.value(robot.code, nullptr);
        row.address = recipient && !recipient->homeAddress.isEmpty() ? recipient->homeAddress : QStringLiteral("-");
        row.recipient = recipient && !recipient->name.isEmpty() ? recipient->name : QStringLiteral("미배정");

        // 오프라인이면 배지 색을 무조건 OFFLINE으로 덮어쓴다 — 긴급/주의 배지는 "지금도 유효한
        // 경보"라는 뜻이라, 통신이 끊겨 최신 상태를 모르는 로봇에 그대로 두면 오해를 준다.
        row.level = robot.isOnline ? (recipient ? riskLevelFromCode(recipient->level) : Tokens::Level::Normal)
                                   : Tokens::Level::Offline;

        // 같은 이유로 통신이 끊긴 로봇은 마지막 배터리 값을 보여주지 않는다(2026-09-14 "LIMO 통신이
        // 끊겼는데 배터리 값이 그대로 뜬다" 리포트). -1이면 표가 "-"로 표시한다.
        row.battery = robot.isOnline ? liveBattery_.value(robot.code, -1) : -1;
        row.medSteps = recipient ? medicationSteps(*recipient) : -1;

        if (!robot.isOnline) row.mission = QStringLiteral("통신 두절");
        else if (patrollingRobotIds_.contains(robot.id)) row.mission = QStringLiteral("순찰/이동 중");
        else row.mission = QStringLiteral("대기 중");

        if (const auto* event = latestUnresolvedByRobot.value(robot.code, nullptr)) {
            row.eventCode = event->eventUid;
            row.eventText = event->eventTypeLabel;
            if (!event->summary.isEmpty()) row.eventText += QStringLiteral(" — ") + event->summary;
        }
        rows_.push_back(row);
    }

    sortRows();
    emit rowsChanged();
}

void P1OverviewModel::sortRows() {
    switch (sortMode_) {
        case P1SortMode::Risk:
            std::stable_sort(rows_.begin(), rows_.end(), [](const P1RobotRow& a, const P1RobotRow& b) {
                return static_cast<int>(a.level) > static_cast<int>(b.level);
            });
            break;
        case P1SortMode::RobotId:
            std::stable_sort(rows_.begin(), rows_.end(),
                             [](const P1RobotRow& a, const P1RobotRow& b) { return a.id < b.id; });
            break;
        case P1SortMode::Name:
            std::stable_sort(rows_.begin(), rows_.end(),
                             [](const P1RobotRow& a, const P1RobotRow& b) { return a.recipient < b.recipient; });
            break;
    }
}

QVector<P1Kpi> P1OverviewModel::kpis() const {
    int onlineRobots = 0, offlineRobots = 0;
    for (const auto& robot : robots_) {
        if (robot.isOnline) ++onlineRobots; else ++offlineRobots;
    }

    int unresolvedEmergency = 0;
    for (const auto& event : events_) {
        if (!event.resolved && event.severity == QStringLiteral("emergency")) ++unresolvedEmergency;
    }

    int medMissedRecipients = 0;
    for (const auto& recipient : recipients_) {
        for (const auto& med : recipient.medsToday) {
            if (med.state == QStringLiteral("missed")) {
                ++medMissedRecipients;
                break;
            }
        }
    }

    // 2026-09-12 — "웨어러블 이상" 카드는 뺐다. 이 프로젝트에 웨어러블 기기·데이터 자체가 없다.
    return {
        {QStringLiteral("가동 로봇"), QStringLiteral("%1/%2").arg(onlineRobots).arg(robots_.size()), nullptr},
        {QStringLiteral("미처리 경보"), QString::number(unresolvedEmergency),
         unresolvedEmergency > 0 ? Tokens::Prop::EMERGENCY : nullptr},
        {QStringLiteral("복약 미수령"), QString::number(medMissedRecipients),
         medMissedRecipients > 0 ? Tokens::Prop::ATTENTION : nullptr},
        {QStringLiteral("통신 두절"), QString::number(offlineRobots), nullptr},
    };
}

QVector<P1AlertRow> P1OverviewModel::alerts() const {
    // GET /events 응답 순서를 믿지 않고(서버가 정렬을 보장 안 함) occurredAt 내림차순으로 다시 정렬한다.
    QVector<ApiClient::EventInfo> sorted = events_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const ApiClient::EventInfo& a, const ApiClient::EventInfo& b) {
        return a.occurredAt > b.occurredAt;
    });

    // events_는 날짜 제한 없는 전체 이력이라 개수로만 자르면 오늘 이벤트가 적은 날 어제 이전 것이
    // 섞여 들어온다("실시간 알림" 이름과 안 맞음, 2026-09-14 리포트). 미해결 이벤트는 날짜와 무관하게
    // 계속 보여줘야 하므로 events_ 자체는 두고, 이 피드에서만 오늘(로컬 기준)로 거른다.
    constexpr int kMaxAlerts = 20;
    const QDate day = today();
    QVector<P1AlertRow> alerts;
    for (const auto& event : sorted) {
        if (event.occurredAt.toLocalTime().date() != day) continue;
        if (alerts.size() >= kMaxAlerts) break;

        const char* level = Tokens::Prop::INFO;
        if (event.severity == QStringLiteral("emergency")) level = Tokens::Prop::EMERGENCY;
        else if (event.severity == QStringLiteral("attention")) level = Tokens::Prop::ATTENTION;

        const QString who = !event.robotCode.isEmpty() ? event.robotCode : event.recipientName;
        QString text = event.eventTypeLabel;
        if (!event.summary.isEmpty()) text += QStringLiteral(" — ") + event.summary;
        if (!who.isEmpty()) text = who + QStringLiteral(" ") + text;

        alerts.push_back({event.occurredAt.toString(QStringLiteral("HH:mm:ss")), level, text});
    }
    return alerts;
}

ApiClient::EventInfo P1OverviewModel::eventFromMessage(const QJsonObject& message) {
    ApiClient::EventInfo event;
    event.eventUid = message.value(QStringLiteral("event_uid")).toString();
    event.severity = message.value(QStringLiteral("severity")).toString();
    event.eventType = message.value(QStringLiteral("event_type")).toString();
    event.eventTypeLabel = message.value(QStringLiteral("event_type_label")).toString();
    event.summary = message.value(QStringLiteral("summary")).toString();
    event.occurredAt = parseIsoDateTime(message.value(QStringLiteral("occurred_at")).toString());
    event.resolved = message.value(QStringLiteral("resolved")).toBool();
    event.robotCode = message.value(QStringLiteral("robot_code")).toString();
    event.recipientName = message.value(QStringLiteral("recipient_name")).toString();
    return event;
}
