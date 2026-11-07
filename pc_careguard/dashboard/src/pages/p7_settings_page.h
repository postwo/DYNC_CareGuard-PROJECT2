#pragma once
#include <QString>
#include <QWidget>

class QTabWidget;
class WsClient;
class P7Api;
class P7RecipientDirectory;
class P7RobotTab;
class P7AccountTab;
class P7ThresholdTab;

// P7 시스템 설정 — 로봇 관리 / 계정 관리 / 임계값·알림 3탭.
// 사용자 로그·임계값·알림 로그·경로순찰 로그 3탭은 2026-09-11 사이드바 개편으로
// 별도 "로그" 항목(P8LogsPage)으로 분리했다.
// CareGuard_DESIGN_SYSTEM.md §6 (idx 6), §7 참고.
//
// 이 클래스는 세 탭을 조립하고 권한(역할)에 따라 보이는 탭을 정하는 일만 한다. 각 탭의 화면과
// 서버 호출은 pages/p7/ 아래 탭 클래스가 맡는다:
//   P7RobotTab      로봇 목록·등록·수정·등록 해제
//   P7AccountTab    계정 목록·등록·수정·비밀번호·활성화·삭제·담당 배정
//   P7ThresholdTab  로봇별 임계값 조회·저장
// 세 탭이 공유하는 것은 P7Api(REST 호출)와 P7RecipientDirectory(대상자 목록)다.
class P7SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit P7SettingsPage(QWidget* parent = nullptr);

    // 로그인 성공 시 main.cpp -> MainWindow를 거쳐 실제 로그인 ID를 전달받는다.
    // 계정 삭제 체크박스/버튼 노출 판단의 기준이 된다.
    void setCurrentUserLoginId(const QString& loginId);

    // role=operator면 로봇 관리·계정 관리 탭을 숨기고 임계값·알림 탭만 조회 전용으로 연다
    // (요청 사양: operator는 이 페이지에서 탭이 하나뿐이고 읽기 전용). admin은 전체 그대로.
    void setCurrentUserRole(const QString& role);

    // MainWindow가 앱 전체에서 공유하는 WsClient 하나를 내려준다 — 페이지마다 자기
    // WsClient를 따로 만들면 /ws/dashboard 소켓이 페이지 수만큼 중복으로 열린다.
    void setWsClient(WsClient* client);

private:
    enum TabIndex { RobotTabIndex = 0, AccountTabIndex = 1, ThresholdTabIndex = 2 };

    P7Api* api_ = nullptr;
    P7RecipientDirectory* recipients_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    P7RobotTab* robotTab_ = nullptr;
    P7AccountTab* accountTab_ = nullptr;
    P7ThresholdTab* thresholdTab_ = nullptr;
    WsClient* wsClient_ = nullptr;
};
