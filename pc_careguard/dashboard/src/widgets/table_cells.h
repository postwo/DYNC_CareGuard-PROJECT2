#pragma once
#include <QColor>
#include <QString>

#include "theme/tokens.h"

class QLabel;
class QWidget;

// 표(QTableWidget) 셀에 넣는 위젯을 만드는 공용 함수. P1·P7이 같은 코드를 각자 복사해 쓰던 것을
// 한곳으로 모았다.
namespace cells {

// 셀 위젯을 행 배경색과 맞춘 래퍼에 담아 가운데 정렬한다(셀 위젯은 아이템 배경을 상속하지 않는다).
QWidget* wrapCellWidget(QWidget* inner, const QColor& rowBg = Tokens::BG_SURFACE);

QLabel* makeBadge(const char* level, const QString& text);

// 값이 없는 칸에 쓰는 흐린 대시("-").
QLabel* mutedDash();

}  // namespace cells
