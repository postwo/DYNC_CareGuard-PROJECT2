#include "p7_api.h"

#include <QJsonArray>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

QString P7Api::Result::errorMessage() const {
    if (jsonValid && json.isObject()) {
        const QJsonValue detail = json.object().value(QStringLiteral("detail"));
        if (detail.isString() && !detail.toString().isEmpty()) return detail.toString();
        // FastAPI 입력 검증 오류(422)는 detail이 오류 객체 배열이다 — 그대로 보여주면 읽을 수 없다.
        if (detail.isArray()) return QStringLiteral("입력값이 올바르지 않습니다");
    }
    if (status == 0) return QStringLiteral("서버에 연결할 수 없습니다");
    return QStringLiteral("요청을 처리하지 못했습니다 (HTTP %1)").arg(status);
}

P7Api::P7Api(const QString& serverUrl, QObject* parent)
    : QObject(parent), serverUrl_(serverUrl), manager_(new QNetworkAccessManager(this)) {}

void P7Api::get(const QString& path, QObject* context, Callback callback) {
    send(Verb::Get, path, nullptr, context, std::move(callback));
}

void P7Api::post(const QString& path, const QJsonObject& body, QObject* context, Callback callback) {
    send(Verb::Post, path, &body, context, std::move(callback));
}

void P7Api::put(const QString& path, const QJsonObject& body, QObject* context, Callback callback) {
    send(Verb::Put, path, &body, context, std::move(callback));
}

void P7Api::patch(const QString& path, const QJsonObject& body, QObject* context, Callback callback) {
    send(Verb::Patch, path, &body, context, std::move(callback));
}

void P7Api::del(const QString& path, QObject* context, Callback callback) {
    send(Verb::Delete, path, nullptr, context, std::move(callback));
}

void P7Api::send(Verb verb, const QString& path, const QJsonObject* body, QObject* context, Callback callback) {
    QNetworkRequest request(QUrl(serverUrl_ + path));
    QByteArray payload;
    if (body != nullptr) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        payload = QJsonDocument(*body).toJson();
    }

    QNetworkReply* reply = nullptr;
    switch (verb) {
        case Verb::Get: reply = manager_->get(request); break;
        case Verb::Post: reply = manager_->post(request, payload); break;
        case Verb::Put: reply = manager_->put(request, payload); break;
        case Verb::Patch: reply = manager_->sendCustomRequest(request, "PATCH", payload); break;
        case Verb::Delete: reply = manager_->deleteResource(request); break;
    }

    // reply는 어떤 경로로 끝나든(성공·실패·context 소멸) 반드시 deleteLater 된다.
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, context, [reply, callback = std::move(callback)] {
        Result result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        if (!raw.isEmpty()) {
            QJsonParseError parseError;
            result.json = QJsonDocument::fromJson(raw, &parseError);
            result.jsonValid = (parseError.error == QJsonParseError::NoError);
        }
        callback(result);
    });
}
