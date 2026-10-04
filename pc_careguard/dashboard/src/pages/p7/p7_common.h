#pragma once
#include <QString>

class QFrame;
class QLabel;
class QWidget;

// P7 세 탭이 같이 쓰는 작은 UI 조립 함수들. 예전엔 p7_settings_page.cpp 익명 네임스페이스에
// 있어서 탭을 파일로 나누려면 먼저 이걸 꺼내야 했다.
namespace p7ui {

QFrame* card(QWidget* parent = nullptr);
QLabel* sectionLabel(const QString& text);

// 목록 헤더행(제목 + 선택적 등록 버튼). 제목 라벨과 버튼은 findChild로 꺼내 쓴다.
QWidget* buildListHeader(const QString& title, const QString& actionText = QString());

}  // namespace p7ui
