#pragma once
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;

// P7 설정 페이지의 세 탭이 공유하는 REST 호출 도우미.
//
// 예전엔 탭마다(정확히는 함수마다) 같은 코드가 반복됐다 — 요청 만들기, finished 연결, 상태코드
// 비교, 오류 본문에서 detail 꺼내기, deleteLater 호출. 반복된 만큼 빠뜨리기 쉬웠고(비밀번호
// 변경·계정 삭제는 서버가 준 detail을 버리고 항상 "서버에 연결할 수 없습니다"만 보여줬다),
// 응답이 오기 전에 탭이 사라질 때를 처리하는 곳도 없었다. 그 반복을 이 클래스 하나로 모은다.
class P7Api : public QObject {
    Q_OBJECT
public:
    // 서버 응답 한 건. 네트워크 자체가 실패하면 status == 0.
    struct Result {
        int status = 0;
        QJsonDocument json;       // body가 올바른 JSON일 때만 유효
        bool jsonValid = false;

        bool is(int expected) const { return status == expected; }
        bool isOneOf(int a, int b) const { return status == a || status == b; }
        // 사용자에게 그대로 보여줄 실패 사유. 서버가 준 detail(문자열)을 우선하고,
        // 검증 오류(detail이 배열)·연결 실패·그 밖의 HTTP 오류는 각각 알맞은 문구로 바꾼다.
        QString errorMessage() const;
    };

    using Callback = std::function<void(const Result&)>;

    explicit P7Api(const QString& serverUrl, QObject* parent = nullptr);

    // context가 사라지면(탭이 먼저 파괴되는 등) 콜백은 호출되지 않는다.
    void get(const QString& path, QObject* context, Callback callback);
    void post(const QString& path, const QJsonObject& body, QObject* context, Callback callback);
    void put(const QString& path, const QJsonObject& body, QObject* context, Callback callback);
    void patch(const QString& path, const QJsonObject& body, QObject* context, Callback callback);
    void del(const QString& path, QObject* context, Callback callback);

private:
    enum class Verb { Get, Post, Put, Patch, Delete };
    void send(Verb verb, const QString& path, const QJsonObject* body, QObject* context, Callback callback);

    QString serverUrl_;
    QNetworkAccessManager* manager_;
};
