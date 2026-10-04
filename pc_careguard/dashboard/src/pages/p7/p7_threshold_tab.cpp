#include "p7_threshold_tab.h"

#include <QAbstractItemView>  // dropdown_fix_combo_box.h가 직접 include하지 않아 먼저 필요하다
#include <QComboBox>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "p7_api.h"
#include "p7_common.h"
#include "theme/tokens.h"
#include "widgets/dropdown_fix_combo_box.h"

namespace {

// 서버 schemas.py::RobotThresholds의 허용 범위와 같다 — 서버가 422로 되돌리기 전에 화면에서 먼저
// 어느 칸이 잘못됐는지 알려준다.
struct FieldRule {
    const char* label;
    int min;
    int max;
};
constexpr FieldRule kBatteryAttention{"배터리 주의", 0, 100};
constexpr FieldRule kBatteryReturn{"배터리 복귀", 0, 100};
constexpr FieldRule kVoiceWait{"음성 응답 대기", 1, 120};
constexpr FieldRule kMedMissedWait{"미수령 판정 시간", 1, 86400};
constexpr FieldRule kMedReannounce{"재안내 횟수", 0, 20};

QLineEdit* thresholdField(const QString& value, int width = 44) {
    auto* edit = new QLineEdit(value);
    edit->setFixedWidth(width);
    edit->setAlignment(Qt::AlignRight);
    return edit;
}

QWidget* thresholdRow(const QString& label, QLineEdit* field, const QString& unit, QLineEdit* field2 = nullptr) {
    auto* row = new QWidget();
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* labelWidget = new QLabel(label, row);
    labelWidget->setProperty("role", "secondary");
    layout->addWidget(labelWidget, /*stretch=*/1);
    layout->addWidget(field);
    if (field2 != nullptr) {
        auto* slash = new QLabel(QStringLiteral("/"), row);
        slash->setProperty("role", "muted");
        layout->addWidget(slash);
        layout->addWidget(field2);
    }
    auto* unitLabel = new QLabel(unit, row);
    unitLabel->setProperty("role", "muted");
    layout->addWidget(unitLabel);
    return row;
}

QVBoxLayout* cardLayout(QFrame* card) {
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING,
                               Tokens::Layout::CARD_PADDING, Tokens::Layout::CARD_PADDING);
    layout->setSpacing(8);
    return layout;
}

// 로봇 선택 팝업 전용 — 카드 위에 "얹힌" 느낌이 나도록 은은한 그림자를 더한다(Tokens SHADOW_MD와
// 같은 값, CareGuard_DESIGN_SYSTEM.md §3 "드롭다운"). 다른 콤보 박스엔 영향 없게 이 파일 안에서만 쓴다.
class ShadowedDropDownComboBox : public DropDownFixComboBox {
public:
    using DropDownFixComboBox::DropDownFixComboBox;
    void showPopup() override {
        QWidget* popup = view()->window();
        // WA_TranslucentBackground는 네이티브 창이 만들어지기 전에 설정해야 효과가 있다.
        popup->setAttribute(Qt::WA_TranslucentBackground);
        DropDownFixComboBox::showPopup();
        auto* shadow = new QGraphicsDropShadowEffect(popup);
        shadow->setBlurRadius(24);
        shadow->setOffset(0, 4);
        shadow->setColor(QColor(22, 32, 43, 26));
        popup->setGraphicsEffect(shadow);
    }
};

// 칸의 내용을 정수로 읽고 범위를 확인한다. 예전엔 toInt()가 잘못된 입력(빈 칸·문자)에 조용히
// 0을 돌려줘서, 오타 하나로 배터리 복귀 임계값 0%가 서버에 저장될 수 있었다.
bool readField(QLineEdit* edit, const FieldRule& rule, int* out, QString* error) {
    bool ok = false;
    const int value = edit->text().trimmed().toInt(&ok);
    if (!ok) {
        *error = QStringLiteral("%1: 숫자를 입력하세요").arg(QString::fromUtf8(rule.label));
        return false;
    }
    if (value < rule.min || value > rule.max) {
        *error = QStringLiteral("%1: %2~%3 사이로 입력하세요").arg(QString::fromUtf8(rule.label)).arg(rule.min).arg(rule.max);
        return false;
    }
    *out = value;
    return true;
}

}  // namespace

P7ThresholdTab::P7ThresholdTab(P7Api* api, QWidget* parent) : QWidget(parent), api_(api) {
    buildUi();
}

void P7ThresholdTab::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(Tokens::Layout::CARD_GAP);

    // 임계값은 로봇별로 저장되므로 어떤 로봇을 보고/고칠지 먼저 고른다.
    auto* selectColumn = new QWidget(this);
    auto* selectLayout = new QVBoxLayout(selectColumn);
    selectLayout->setContentsMargins(0, 0, 0, 0);
    selectLayout->setSpacing(4);
    selectLayout->addWidget(p7ui::sectionLabel(QStringLiteral("로봇 선택")));
    robotCombo_ = new ShadowedDropDownComboBox(selectColumn);
    robotCombo_->setObjectName(QStringLiteral("thresholdRobotCombo"));
    robotCombo_->setMinimumWidth(220);
    connect(robotCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) fetchForCurrentRobot();
    });
    selectLayout->addWidget(robotCombo_);
    layout->addWidget(selectColumn);

    auto* cardsRow = new QWidget(this);
    auto* cardsLayout = new QHBoxLayout(cardsRow);
    cardsLayout->setContentsMargins(0, 0, 0, 0);
    cardsLayout->setSpacing(Tokens::Layout::CARD_GAP);

    auto* robotCard = p7ui::card(cardsRow);
    auto* robotLayout = cardLayout(robotCard);
    robotLayout->addWidget(p7ui::sectionLabel(QStringLiteral("로봇 공통")));
    batteryAttentionEdit_ = thresholdField("30");
    batteryReturnEdit_ = thresholdField("28");
    robotLayout->addWidget(thresholdRow(QStringLiteral("배터리 주의 / 복귀"), batteryAttentionEdit_,
                                        QStringLiteral("%"), batteryReturnEdit_));
    voiceWaitEdit_ = thresholdField("8");
    robotLayout->addWidget(thresholdRow(QStringLiteral("음성 응답 대기"), voiceWaitEdit_, QStringLiteral("초")));
    cardsLayout->addWidget(robotCard, 1);

    auto* medCard = p7ui::card(cardsRow);
    auto* medLayout = cardLayout(medCard);
    medLayout->addWidget(p7ui::sectionLabel(QStringLiteral("복약 공통")));
    medMissedWaitEdit_ = thresholdField("600");
    medLayout->addWidget(thresholdRow(QStringLiteral("미수령 판정 시간"), medMissedWaitEdit_, QStringLiteral("초")));
    medReannounceEdit_ = thresholdField("2");
    medLayout->addWidget(thresholdRow(QStringLiteral("재안내 횟수"), medReannounceEdit_, QStringLiteral("회")));
    cardsLayout->addWidget(medCard, 1);
    layout->addWidget(cardsRow);

    auto* buttonRow = new QWidget(this);
    auto* buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(10);
    buttonLayout->addStretch();

    saveButton_ = new QPushButton(QStringLiteral("이 로봇에 저장"), buttonRow);
    saveButton_->setProperty("variant", "primary");
    connect(saveButton_, &QPushButton::clicked, this, &P7ThresholdTab::saveForCurrentRobot);
    buttonLayout->addWidget(saveButton_);

    applyAllButton_ = new QPushButton(QStringLiteral("전체 로봇에 적용"), buttonRow);
    applyAllButton_->setProperty("variant", "secondary");
    connect(applyAllButton_, &QPushButton::clicked, this, &P7ThresholdTab::applyToAllRobots);
    buttonLayout->addWidget(applyAllButton_);

    layout->addWidget(buttonRow);
    layout->addStretch();
}

void P7ThresholdTab::setRobots(const QVector<P7Robot>& robots) {
    bool sameList = robots.size() == robots_.size();
    for (int i = 0; sameList && i < robots.size(); ++i) {
        sameList = robots[i].dbId == robots_[i].dbId && robots[i].id == robots_[i].id;
    }
    if (sameList) return;

    robots_ = robots;
    const int previousDbId = robotCombo_->count() > 0 ? robotCombo_->currentData().toInt() : -1;
    robotCombo_->blockSignals(true);
    robotCombo_->clear();
    int restoreIndex = -1;
    for (const P7Robot& robot : robots_) {
        robotCombo_->addItem(robot.id, robot.dbId);
        if (robot.dbId == previousDbId) restoreIndex = robotCombo_->count() - 1;
    }
    robotCombo_->blockSignals(false);
    if (robotCombo_->count() == 0) return;
    robotCombo_->setCurrentIndex(restoreIndex >= 0 ? restoreIndex : 0);
    fetchForCurrentRobot();
}

void P7ThresholdTab::setReadOnly(bool readOnly) {
    for (QLineEdit* edit : {batteryAttentionEdit_, batteryReturnEdit_, voiceWaitEdit_, medMissedWaitEdit_,
                            medReannounceEdit_}) {
        edit->setReadOnly(readOnly);
        edit->setFocusPolicy(readOnly ? Qt::NoFocus : Qt::StrongFocus);
        edit->setProperty("variant", readOnly ? QStringLiteral("plain") : QVariant());
        edit->style()->unpolish(edit);
        edit->style()->polish(edit);
    }
    saveButton_->setVisible(!readOnly);
    applyAllButton_->setVisible(!readOnly);
}

void P7ThresholdTab::fetchForCurrentRobot() {
    const int robotDbId = robotCombo_->currentData().toInt();
    api_->get(QStringLiteral("/robots/%1/thresholds").arg(robotDbId), this,
              [this, robotDbId](const P7Api::Result& result) {
                  // 로봇을 빠르게 바꾸면 먼저 보낸 응답이 나중에 도착해 지금 고른 로봇의 값을 덮어쓸 수
                  // 있다 — 응답이 지금 선택한 로봇의 것일 때만 반영한다.
                  if (robotCombo_->currentData().toInt() != robotDbId) return;
                  if (!result.is(200) || !result.json.isObject()) return;
                  const QJsonObject body = result.json.object();
                  Values values;
                  values.batteryAttention = body.value("battery_attention_pct").toInt(values.batteryAttention);
                  values.batteryReturn = body.value("battery_return_pct").toInt(values.batteryReturn);
                  values.voiceWait = body.value("voice_wait_sec").toInt(values.voiceWait);
                  values.medMissedWait = body.value("med_missed_wait_sec").toInt(values.medMissedWait);
                  values.medReannounce = body.value("med_reannounce_count").toInt(values.medReannounce);
                  showValues(values);
              });
}

void P7ThresholdTab::showValues(const Values& values) {
    batteryAttentionEdit_->setText(QString::number(values.batteryAttention));
    batteryReturnEdit_->setText(QString::number(values.batteryReturn));
    voiceWaitEdit_->setText(QString::number(values.voiceWait));
    medMissedWaitEdit_->setText(QString::number(values.medMissedWait));
    medReannounceEdit_->setText(QString::number(values.medReannounce));
}

bool P7ThresholdTab::readValues(Values* out) {
    QString error;
    Values values;
    const bool ok = readField(batteryAttentionEdit_, kBatteryAttention, &values.batteryAttention, &error) &&
                    readField(batteryReturnEdit_, kBatteryReturn, &values.batteryReturn, &error) &&
                    readField(voiceWaitEdit_, kVoiceWait, &values.voiceWait, &error) &&
                    readField(medMissedWaitEdit_, kMedMissedWait, &values.medMissedWait, &error) &&
                    readField(medReannounceEdit_, kMedReannounce, &values.medReannounce, &error);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("입력값 확인"), error);
        return false;
    }
    *out = values;
    return true;
}

QJsonObject P7ThresholdTab::toJson(const Values& values) const {
    return QJsonObject{
        {"battery_attention_pct", values.batteryAttention},
        {"battery_return_pct", values.batteryReturn},
        {"voice_wait_sec", values.voiceWait},
        {"med_missed_wait_sec", values.medMissedWait},
        {"med_reannounce_count", values.medReannounce},
    };
}

void P7ThresholdTab::saveForCurrentRobot() {
    if (robotCombo_->count() == 0) return;
    Values values;
    if (!readValues(&values)) return;

    const int robotDbId = robotCombo_->currentData().toInt();
    api_->put(QStringLiteral("/robots/%1/thresholds").arg(robotDbId), toJson(values), this,
              [this](const P7Api::Result& result) {
                  if (!result.is(204)) QMessageBox::warning(this, QStringLiteral("임계값 저장 실패"), result.errorMessage());
              });
}

void P7ThresholdTab::applyToAllRobots() {
    Values values;
    if (!readValues(&values)) return;

    api_->put(QStringLiteral("/robots/thresholds/apply-all"), toJson(values), this, [this](const P7Api::Result& result) {
        if (!result.is(204)) {
            QMessageBox::warning(this, QStringLiteral("전체 적용 실패"), result.errorMessage());
            return;
        }
        QMessageBox::information(this, QStringLiteral("전체 적용"), QStringLiteral("현재 등록된 모든 로봇에 임계값을 적용했습니다"));
    });
}
