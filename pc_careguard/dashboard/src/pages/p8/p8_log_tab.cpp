#include "p8_log_tab.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QDateEdit>
#include <QDateTime>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "theme/calendar_style.h"
#include "theme/tokens.h"
#include "widgets/careguard_date_edit.h"
#include "widgets/live_search_line_edit.h"

namespace {

QLabel* makeMutedLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", "muted");
    return label;
}

void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

}  // namespace

QString formatLogTime(const QString& iso) {
    QDateTime at = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!at.isValid()) at = QDateTime::fromString(iso, Qt::ISODate);
    return at.toString(QStringLiteral("MM.dd HH:mm"));
}

P8LogTab::P8LogTab(const P8LogTabSpec& spec, QNetworkAccessManager* net, const QString& serverUrl, QWidget* parent)
    : QWidget(parent), spec_(spec), net_(net), serverUrl_(serverUrl) {
    buildUi();
    initializing_ = false;
}

void P8LogTab::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(buildFilterRow());

    // 조회에 실패하면 표가 비는 이유를 알 수 있게 알린다 — 예전엔 "기록 없음"과 "서버에 못 닿음"이
    // 똑같이 빈 표로 보였다.
    errorLabel_ = new QLabel(QStringLiteral("로그를 불러오지 못했습니다 — 서버 연결을 확인하세요."), this);
    errorLabel_->setProperty("role", "muted");
    errorLabel_->setVisible(false);
    layout->addWidget(errorLabel_);

    table_ = new QTableWidget(this);
    table_->setColumnCount(spec_.headers.size());
    table_->setHorizontalHeaderLabels(spec_.headers);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setShowGrid(false);

    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    if (spec_.stretchColumn >= 0) header->setSectionResizeMode(spec_.stretchColumn, QHeaderView::Stretch);
    for (int column = 0; column < spec_.columnWidths.size(); ++column) {
        if (spec_.columnWidths[column] > 0) table_->setColumnWidth(column, spec_.columnWidths[column]);
    }
    layout->addWidget(table_, /*stretch=*/1);
}

QWidget* P8LogTab::buildFilterRow() {
    auto* filterRow = new QWidget(this);
    auto* filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);

    // 기간 세그먼트 + 날짜 범위 + 검색 — P2EventPage::buildFilterBar()와 같은 구성.
    const QDate today = QDate::currentDate();

    auto* periodSeg = new QWidget(filterRow);
    auto* periodLayout = new QHBoxLayout(periodSeg);
    periodLayout->setContentsMargins(0, 0, 0, 0);
    periodLayout->setSpacing(0);

    auto* todayButton = new QPushButton(QStringLiteral("오늘"), periodSeg);
    auto* weekButton = new QPushButton(QStringLiteral("7일"), periodSeg);
    auto* monthButton = new QPushButton(QStringLiteral("30일"), periodSeg);
    customButton_ = new QPushButton(QStringLiteral("직접"), periodSeg);
    todayButton->setProperty("pos", "first");
    customButton_->setProperty("pos", "last");

    auto* periodGroup = new QButtonGroup(this);
    periodGroup->setExclusive(true);
    for (auto* button : {todayButton, weekButton, monthButton, customButton_}) {
        button->setObjectName("segBtn");
        button->setCheckable(true);
        periodLayout->addWidget(button);
        periodGroup->addButton(button);
    }
    weekButton->setChecked(true);  // 기존 "최근 7일" 기본값을 그대로 유지
    filterLayout->addWidget(periodSeg);

    connect(todayButton, &QPushButton::clicked, this, [this] { applyPeriodPreset(0); });
    connect(weekButton, &QPushButton::clicked, this, [this] { applyPeriodPreset(6); });
    connect(monthButton, &QPushButton::clicked, this, [this] { applyPeriodPreset(29); });
    connect(customButton_, &QPushButton::clicked, this, [this] { applyPeriodPreset(-1); });

    startEdit_ = new CareGuardDateEdit(today, filterRow);
    CareGuard::applyCareGuardDateEdit(startEdit_);
    filterLayout->addWidget(startEdit_);
    filterLayout->addWidget(makeMutedLabel(QStringLiteral("~"), filterRow));
    endEdit_ = new CareGuardDateEdit(today, filterRow);
    CareGuard::applyCareGuardDateEdit(endEdit_);
    filterLayout->addWidget(endEdit_);

    connect(startEdit_, &QDateEdit::dateChanged, this, &P8LogTab::onDateRangeChanged);
    connect(endEdit_, &QDateEdit::dateChanged, this, &P8LogTab::onDateRangeChanged);
    // 읽기 전용 칸은 클릭이 편집으로 이어지지 않으니, 클릭 자체를 잡아 "직접"으로 전환한다.
    startEdit_->installEventFilter(this);
    endEdit_->installEventFilter(this);

    applyPeriodPreset(6);  // 초기 상태: 최근 7일, 읽기 전용

    searchEdit_ = new LiveSearchLineEdit(filterRow);
    searchEdit_->setPlaceholderText(spec_.searchPlaceholder);
    connect(searchEdit_, &LiveSearchLineEdit::liveTextChanged, this, [this](const QString& text) {
        searchText_ = text;
        refresh();
    });
    filterLayout->addWidget(searchEdit_, /*stretch=*/1);
    return filterRow;
}

bool P8LogTab::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress && (watched == startEdit_ || watched == endEdit_) &&
        startEdit_->isReadOnly()) {
        applyPeriodPreset(-1);
    }
    return QWidget::eventFilter(watched, event);
}

void P8LogTab::applyPeriodPreset(int spanDays) {
    if (spanDays >= 0) {
        const QDate today = QDate::currentDate();
        startEdit_->setDate(today.addDays(-spanDays));
        endEdit_->setDate(today);
        setDateEditsReadOnly(true);
    } else {
        setDateEditsReadOnly(false);
        if (!customButton_->isChecked()) customButton_->setChecked(true);  // 날짜 칸을 직접 클릭해 진입한 경우
    }
}

void P8LogTab::setDateEditsReadOnly(bool readOnly) {
    for (auto* edit : {startEdit_, endEdit_}) {
        edit->setReadOnly(readOnly);
        edit->setProperty("state", readOnly ? QStringLiteral("off") : QVariant());
        repolish(edit);
    }
}

void P8LogTab::onDateRangeChanged() {
    // 시작이 끝보다 늦으면 끝을 시작에 맞춰 밀어준다(P2EventPage::onDateRangeChanged와 같은 규칙).
    if (startEdit_->date() > endEdit_->date()) {
        endEdit_->setDate(startEdit_->date());
        return;  // setDate가 다시 dateChanged를 내서 아래 조회까지 이어진다.
    }
    refresh();
}

void P8LogTab::refresh() {
    // 생성 중 초기 날짜 설정이 일으키는 dateChanged는 무시한다 — 최초 조회는 호출부가 명시적으로 한다.
    if (initializing_) return;

    QUrl url(serverUrl_ + spec_.endpoint);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("start"), startEdit_->date().toString(Qt::ISODate));
    query.addQueryItem(QStringLiteral("end"), endEdit_->date().toString(Qt::ISODate));
    if (!searchText_.trimmed().isEmpty()) query.addQueryItem(QStringLiteral("q"), searchText_.trimmed());
    url.setQuery(query);

    const quint64 requestId = ++latestRequest_;
    QNetworkReply* reply = net_->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId] {
        if (requestId != latestRequest_) return;  // 더 최근 조회가 이미 나갔다 — 낡은 응답은 버린다

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (status != 200 || parseError.error != QJsonParseError::NoError || !doc.isArray()) {
            showRows({}, {});
            showError(true);
            return;
        }

        QVector<QStringList> rows;
        QVector<bool> highlights;
        for (const QJsonValue& value : doc.array()) {
            const QJsonObject obj = value.toObject();
            QStringList cells = spec_.toCells(obj);
            while (cells.size() < spec_.headers.size()) cells << QString();  // 매핑이 열보다 짧아도 안전하게
            rows.push_back(cells);
            highlights.push_back(obj.value("highlight").toBool());
        }
        showRows(rows, highlights);
        showError(false);
    });
}

void P8LogTab::showError(bool failed) {
    errorLabel_->setVisible(failed);
}

void P8LogTab::showRows(const QVector<QStringList>& rows, const QVector<bool>& highlights) {
    table_->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        for (int column = 0; column < spec_.headers.size(); ++column) {
            auto* item = new QTableWidgetItem(rows[r][column]);
            item->setTextAlignment(Qt::AlignCenter);
            if (spec_.monoColumns.contains(column)) item->setFont(QFont(Tokens::Font::MONO));
            if (column == 0) item->setForeground(Tokens::TEXT_SECONDARY);
            if (column == spec_.highlightColumn && highlights[r]) {
                item->setForeground(Tokens::EMERGENCY.main);
                QFont font = item->font();
                font.setWeight(QFont::DemiBold);
                item->setFont(font);
            }
            table_->setItem(r, column, item);
        }
    }
}
