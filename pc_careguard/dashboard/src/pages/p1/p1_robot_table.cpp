#include "p1_robot_table.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

#include "theme/tokens.h"
#include "widgets/table_cells.h"

namespace {

constexpr int kBarColorRole = Qt::UserRole + 1;

void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QColor levelMainColor(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return Tokens::EMERGENCY.main;
        case Tokens::Level::Attention: return Tokens::ATTENTION.main;
        case Tokens::Level::Offline:   return Tokens::OFFLINE.main;
        case Tokens::Level::Normal:    return Tokens::TEXT_PRIMARY;
    }
    return Tokens::TEXT_PRIMARY;
}

QColor levelSoftColor(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return Tokens::EMERGENCY.soft;
        case Tokens::Level::Attention: return Tokens::ATTENTION.soft;
        case Tokens::Level::Offline:   return Tokens::OFFLINE.soft;
        case Tokens::Level::Normal:    return Tokens::BG_SURFACE;
    }
    return Tokens::BG_SURFACE;
}

QString levelLabel(Tokens::Level level) {
    switch (level) {
        case Tokens::Level::Emergency: return QStringLiteral("긴급");
        case Tokens::Level::Attention: return QStringLiteral("주의");
        case Tokens::Level::Offline:   return QStringLiteral("오프라인");
        case Tokens::Level::Normal:    return QStringLiteral("정상");
    }
    return QString();
}

// "상태" 컬럼 전용: 좌측 4px 상태 바를 셀 위에 덧그린다(행 배경·좌측 바는 QSS가 아닌 델리게이트로
// 구현 — careguard.qss §테이블 주석 참고).
class StatusBarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);

        const QVariant barColor = index.data(kBarColorRole);
        if (!barColor.canConvert<QColor>()) return;
        painter->save();
        painter->fillRect(option.rect.left(), option.rect.top(), Tokens::Layout::STATUS_BAR_W,
                          option.rect.height(), barColor.value<QColor>());
        painter->restore();
    }
};

// 복약 3단계 진행 표시(투약 준비 → 개봉 → 복용 확인). QSS에 없는 전용 위젯이라 직접 그린다.
class MedStepsWidget : public QWidget {
public:
    explicit MedStepsWidget(int completedSteps, QWidget* parent = nullptr)
        : QWidget(parent), completed_(completedSteps) {
        setFixedSize(44, 16);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        constexpr int kDot = 8;
        constexpr int kGap = 6;
        const int totalWidth = kDot * 3 + kGap * 2;
        int x = (width() - totalWidth) / 2;
        const int y = (height() - kDot) / 2;
        for (int i = 0; i < 3; ++i) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(i < completed_ ? Tokens::OK.main : Tokens::BORDER_STRONG);
            painter.drawEllipse(x, y, kDot, kDot);
            x += kDot + kGap;
        }
    }

private:
    int completed_;
};

// 컬럼 번호 — buildUi()의 헤더 순서와 같아야 한다.
enum Column { Status = 0, RobotId, Battery, Recipient, Medication, Mission, Address, Detail, Stop };

}  // namespace

P1RobotTable::P1RobotTable(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void P1RobotTable::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* headerRow = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    auto* title = new QLabel(QStringLiteral("로봇·대상자 현황"), headerRow);
    title->setProperty("role", "h3");
    headerLayout->addWidget(title);
    headerLayout->addStretch();

    auto* sortGroup = new QButtonGroup(headerRow);
    const struct { QString text; P1SortMode mode; const char* pos; } sortSpecs[] = {
        {QStringLiteral("위험순"), P1SortMode::Risk, "first"},
        {QStringLiteral("로봇ID"), P1SortMode::RobotId, ""},
        {QStringLiteral("이름순"), P1SortMode::Name, "last"},
    };
    for (const auto& spec : sortSpecs) {
        auto* button = new QPushButton(spec.text, headerRow);
        button->setObjectName("segBtn");
        button->setProperty("pos", spec.pos);
        button->setCheckable(true);
        button->setChecked(spec.mode == P1SortMode::Risk);
        sortGroup->addButton(button, static_cast<int>(spec.mode));
        headerLayout->addWidget(button);
    }
    connect(sortGroup, &QButtonGroup::idClicked, this,
            [this](int id) { emit sortModeChanged(static_cast<P1SortMode>(id)); });
    layout->addWidget(headerRow);

    table_ = new QTableWidget(this);
    const QStringList headers = {QStringLiteral("상태"),   QStringLiteral("로봇ID"),
                                 QStringLiteral("배터리%"), QStringLiteral("대상자"),
                                 QStringLiteral("복약"),
                                 QStringLiteral("현재 미션"), QStringLiteral("가정(주소)"),
                                 QStringLiteral("액션"),    QStringLiteral("정지")};
    table_->setColumnCount(headers.size());
    table_->setHorizontalHeaderLabels(headers);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(Tokens::Layout::TABLE_ROW_H);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setShowGrid(false);
    table_->setItemDelegateForColumn(Status, new StatusBarDelegate(table_));
    table_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(Address, QHeaderView::Stretch);  // 가정(주소) — 남는 폭을 전부 흡수
    table_->setColumnWidth(Status, 135);
    table_->setColumnWidth(RobotId, 115);
    table_->setColumnWidth(Battery, 155);
    table_->setColumnWidth(Recipient, 135);
    table_->setColumnWidth(Medication, 120);
    table_->setColumnWidth(Mission, 420);
    table_->setColumnWidth(Detail, 80);
    table_->setColumnWidth(Stop, 80);
    layout->addWidget(table_);
}

void P1RobotTable::setRows(const std::vector<P1RobotRow>& rows) {
    table_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) populateRow(i, rows[static_cast<size_t>(i)]);
}

void P1RobotTable::populateRow(int r, const P1RobotRow& row) {
    const bool offline = row.level == Tokens::Level::Offline;
    const QColor rowBg = levelSoftColor(row.level);
    const QColor textColor = offline ? Tokens::OFFLINE.main : Tokens::TEXT_PRIMARY;

    auto addItem = [&](int column, const QString& text, const QColor& color, bool mono = false) {
        auto* item = new QTableWidgetItem(text);
        if (mono) item->setFont(QFont(Tokens::Font::MONO));
        item->setTextAlignment(Qt::AlignCenter);
        item->setBackground(rowBg);
        item->setForeground(color);
        table_->setItem(r, column, item);
        return item;
    };

    auto* statusItem = addItem(Status, levelLabel(row.level), levelMainColor(row.level));
    if (row.level == Tokens::Level::Emergency || row.level == Tokens::Level::Attention) {
        statusItem->setData(kBarColorRole, levelMainColor(row.level));
    }
    addItem(RobotId, row.id, textColor, /*mono=*/true);

    if (row.battery >= 0) {
        auto* bar = new QProgressBar();
        bar->setRange(0, 100);
        bar->setValue(row.battery);
        bar->setTextVisible(false);
        bar->setFixedWidth(70);
        if (row.battery < 20) bar->setProperty("level", Tokens::Prop::EMERGENCY);
        else if (row.battery < 40) bar->setProperty("level", Tokens::Prop::ATTENTION);
        repolish(bar);

        auto* batteryCell = new QWidget();
        auto* batteryLayout = new QHBoxLayout(batteryCell);
        batteryLayout->setContentsMargins(0, 0, 0, 0);
        batteryLayout->setSpacing(6);
        batteryLayout->addWidget(bar);
        auto* batteryLabel = new QLabel(QStringLiteral("%1%").arg(row.battery), batteryCell);
        batteryLabel->setStyleSheet(QStringLiteral("color:%1;").arg(textColor.name()));
        batteryLayout->addWidget(batteryLabel);
        table_->setCellWidget(r, Battery, cells::wrapCellWidget(batteryCell, rowBg));
    } else {
        table_->setCellWidget(r, Battery, cells::wrapCellWidget(cells::mutedDash(), rowBg));
    }

    addItem(Recipient, row.recipient, textColor);

    table_->setCellWidget(r, Medication,
                          cells::wrapCellWidget(row.medSteps >= 0 ? static_cast<QWidget*>(new MedStepsWidget(row.medSteps))
                                                                  : cells::mutedDash(),
                                                rowBg));

    addItem(Mission, row.mission, textColor);
    addItem(Address, row.address, textColor);

    auto* detailButton = new QPushButton(QStringLiteral("상세"));
    detailButton->setObjectName("detailBtn");
    detailButton->setProperty("variant", "secondary");
    connect(detailButton, &QPushButton::clicked, this, [this, row] { emit detailRequested(row); });
    table_->setCellWidget(r, Detail, cells::wrapCellWidget(detailButton, rowBg));

    auto* stopButton = new QPushButton(QStringLiteral("정지"));
    stopButton->setObjectName("rowEstopBtn");
    if (offline) {
        stopButton->setEnabled(false);
        stopButton->setToolTip(QStringLiteral("통신 두절 — 정지 명령 전달 불가"));
    } else {
        connect(stopButton, &QPushButton::clicked, this, [this, code = row.id] { emit stopRequested(code); });
    }
    table_->setCellWidget(r, Stop, cells::wrapCellWidget(stopButton, rowBg));
}
