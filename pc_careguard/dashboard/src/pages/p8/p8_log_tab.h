#pragma once
#include <QDate>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <functional>

class QButtonGroup;
class QDateEdit;
class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QTableWidget;
class LiveSearchLineEdit;

// 서버가 내려주는 시각 문자열(ISO 8601, 초 이하 자릿수가 서버·행마다 다를 수 있다)을 "MM.dd HH:mm"으로.
// 해석할 수 없으면 빈 문자열.
QString formatLogTime(const QString& iso);

// 로그 탭 하나가 무엇을 보여줄지 정하는 값. 세 탭(사용자 로그·임계값/알림 로그·경로순찰 로그)은 필터
// UI·조회·표 그리기가 완전히 같고 아래 값만 다르다 — 예전엔 같은 코드를 세 벌 복사해 갖고 있었다.
struct P8LogTabSpec {
    QString endpoint;           // 예: "/audit-log" — ?start=&end=&q= 를 붙여 조회한다
    QString searchPlaceholder;
    QStringList headers;
    QVector<int> columnWidths;  // headers와 같은 길이. 0이면 기본 폭(늘어나는 열은 stretchColumn)
    int stretchColumn = -1;     // 남는 폭을 흡수할 열
    QSet<int> monoColumns;      // 고정폭 글꼴로 그릴 열
    int highlightColumn = -1;   // 서버가 highlight=true로 표시한 행에서 강조색으로 그릴 열
    // 응답 한 행(JSON)을 열 값들로 바꾼다. 개수는 headers와 같아야 한다.
    std::function<QStringList(const QJsonObject&)> toCells;
};

// 기간 세그먼트(오늘/7일/30일/직접) + 날짜 범위 + 검색 + 표로 이뤄진 로그 조회 탭.
class P8LogTab : public QWidget {
    Q_OBJECT
public:
    P8LogTab(const P8LogTabSpec& spec, QNetworkAccessManager* net, const QString& serverUrl,
             QWidget* parent = nullptr);

    // 지금 적용 중인 기간·검색 필터 그대로 다시 조회한다.
    void refresh();

protected:
    // 기간 세그먼트가 "직접"이 아닐 때 읽기 전용 날짜 칸을 클릭하면 "직접"으로 전환한다.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    QWidget* buildFilterRow();
    void applyPeriodPreset(int spanDays);  // -1 = 직접, 0/6/29 = 오늘/7일/30일
    void setDateEditsReadOnly(bool readOnly);
    void onDateRangeChanged();
    void showRows(const QVector<QStringList>& rows, const QVector<bool>& highlights);
    void showError(bool failed);

    P8LogTabSpec spec_;
    QNetworkAccessManager* net_;
    QString serverUrl_;

    QTableWidget* table_ = nullptr;
    QDateEdit* startEdit_ = nullptr;
    QDateEdit* endEdit_ = nullptr;
    QPushButton* customButton_ = nullptr;
    LiveSearchLineEdit* searchEdit_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QString searchText_;

    // 검색어를 한 글자씩 칠 때마다 요청이 나가므로, 먼저 보낸 응답이 나중에 도착해 최신 결과를 덮어쓸
    // 수 있다 — 가장 마지막에 보낸 요청의 응답만 화면에 반영한다.
    quint64 latestRequest_ = 0;
    bool initializing_ = true;  // 생성 중 초기 날짜 설정이 조회를 일으키지 않게 한다
};
