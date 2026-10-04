#pragma once
#include <QObject>
#include <QString>
#include <QVector>

class P7Api;

// GET /accounts/recipients 응답 한 줄. 계정 수정 모달·로봇 등록 모달의 "담당 대상자"
// 선택지 원본이라 두 탭이 같이 쓴다.
struct P7Recipient {
    int id = -1;
    QString code;         // "C-001"
    QString name;         // "김순자"
    QString label;        // "김순자 (C-001) · R-01"
    int operatorId = -1;  // 현재 담당 계정 id. 미배정이면 -1
};

// 대상자 목록을 한 곳에서 받아 두고 필요한 탭들이 읽는다. 예전엔 페이지 클래스가 recipients_를
// 직접 들고 있어서 로봇 탭과 계정 탭을 나누면 서로의 멤버를 건드려야 했다.
class P7RecipientDirectory : public QObject {
    Q_OBJECT
public:
    explicit P7RecipientDirectory(P7Api* api, QObject* parent = nullptr);

    void refresh();
    const QVector<P7Recipient>& all() const { return recipients_; }

signals:
    void changed();

private:
    P7Api* api_;
    QVector<P7Recipient> recipients_;
};
