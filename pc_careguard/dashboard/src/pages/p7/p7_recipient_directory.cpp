#include "p7_recipient_directory.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtDebug>

#include "p7_api.h"

P7RecipientDirectory::P7RecipientDirectory(P7Api* api, QObject* parent) : QObject(parent), api_(api) {}

void P7RecipientDirectory::refresh() {
    api_->get(QStringLiteral("/accounts/recipients"), this, [this](const P7Api::Result& result) {
        if (!result.is(200)) {
            // 서버에 닿지 않으면 선택지를 비운다 — 오래된 목록으로 잘못된 배정을 하는 것보다 낫다.
            recipients_.clear();
            emit changed();
            return;
        }
        if (!result.json.isArray()) {
            qWarning() << "GET /accounts/recipients 응답이 배열이 아닙니다 — 기존 목록을 유지합니다";
            return;
        }

        QVector<P7Recipient> rows;
        for (const QJsonValue& value : result.json.array()) {
            const QJsonObject obj = value.toObject();
            P7Recipient row;
            row.id = obj.value("id").toInt();
            row.code = obj.value("code").toString();
            row.name = obj.value("name").toString();
            const QString robotCode = obj.value("robot_code").toString();
            row.label = robotCode.isEmpty()
                            ? QStringLiteral("%1 (%2)").arg(row.name, row.code)
                            : QStringLiteral("%1 (%2) · %3").arg(row.name, row.code, robotCode);
            row.operatorId = obj.value("operator_id").isNull() ? -1 : obj.value("operator_id").toInt();
            rows.push_back(row);
        }
        recipients_ = rows;
        emit changed();
    });
}
