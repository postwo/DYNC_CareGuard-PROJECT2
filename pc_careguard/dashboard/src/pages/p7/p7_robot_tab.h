#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>

#include "dialogs/p7_robot_dialog.h"
#include "theme/tokens.h"

class QLabel;
class QTableWidget;
class P7Api;
class P7RecipientDirectory;

// 로봇 관리 탭에 표시하는 로봇 한 대. 임계값 탭의 로봇 선택 콤보도 이 목록을 쓴다.
struct P7Robot {
    int dbId = -1;  // robot.id — 수정·등록 해제 API 호출에 필요
    QString id;     // robot.code, 예: "R-01"
    QString name;
    QString model = QStringLiteral("limo");
    QString ip;
    QString ns;  // ROS 도메인 표시용
    int rosDomainId = 0;
    QString mapRecipient;
    Tokens::Level level = Tokens::Level::Offline;
    QString statusDetail;
    QString recipientLabel = QStringLiteral("미배정");
    QString wsHost = QStringLiteral("192.168.0.9");
    QString connectionDetail;

    bool isOnline() const { return level != Tokens::Level::Offline; }
};

// P7 "로봇 관리" 탭 — 로봇 목록 표시, 등록·수정·등록 해제.
// 로봇 목록의 유일한 소유자이고, 목록이 바뀌면 robotsChanged()로 다른 탭에 알린다.
class P7RobotTab : public QWidget {
    Q_OBJECT
public:
    P7RobotTab(P7Api* api, P7RecipientDirectory* recipients, QWidget* parent = nullptr);

    void refresh();  // GET /robots
    // /ws/dashboard 메시지 중 로봇 관련(robot_updated, robot_status 등)을 처리한다.
    void handleWsMessage(const QJsonObject& message);

    const QVector<P7Robot>& robots() const { return robots_; }

signals:
    void robotsChanged();

private:
    void buildUi();
    void populateRow(int row);
    void refreshTable();
    void handleRobotsReply(const QVector<P7Robot>& rows);
    int indexOfDbId(int dbId) const;

    // 모달은 열린 동안에도 WS 알림으로 목록이 갱신될 수 있어, 행 번호가 아니라 robot.id를
    // 붙잡는다 — 번호를 붙잡으면 그 사이 목록이 줄었을 때 엉뚱한 로봇을 수정·삭제하거나 범위를
    // 벗어난 접근이 된다.
    void openRobotDialog(int dbId);  // dbId < 0이면 신규 등록
    void registerRobot(const RobotDialog::RobotData& data);
    void updateRobot(int robotDbId, const RobotDialog::RobotData& data);
    void assignRecipient(int robotDbId, int recipientId);
    void deregister(int robotDbId);
    void showFailure(const QString& title, const QString& message);

    P7Api* api_;
    P7RecipientDirectory* recipients_;
    QTableWidget* table_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QVector<P7Robot> robots_;
};
