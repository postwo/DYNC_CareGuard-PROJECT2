#!/bin/bash
# 이 로봇(LIMO)에 CareGuard 자동 기동 systemd 서비스를 설치한다.
#
# 사용법 (로봇 코드를 소문자로, 이 스크립트가 있는 디렉토리에서 실행):
#   ./install.sh r07
#
# careguard_turtlebot_agent/systemd/install.sh와 같은 관례를 따르되, LIMO는
# bringup·uplink 외에 downlink(서버->로봇 명령, 배터리 자동 복귀 포함)까지
# 3개 서비스라서 이름 패턴(careguard-*-<code>.service)으로 전부 찾아 설치한다.
set -euo pipefail

CODE="${1:?사용법: ./install.sh r07 (소문자 로봇 코드)}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

UNITS=("$SCRIPT_DIR"/careguard-*-"${CODE}".service)
if [ ! -e "${UNITS[0]}" ]; then
    echo "에러: $SCRIPT_DIR 에 ${CODE} 로봇용 서비스 파일이 없음" >&2
    exit 1
fi

# systemctl에는 유닛 이름을 인자 하나하나로 넘겨야 한다. "$(basename -a ...)"처럼 따옴표로 감싸면
# 줄바꿈으로 이어진 문자열 하나가 돼 첫 유닛만 처리되거나 "유닛 없음"으로 실패한다.
mapfile -t UNIT_NAMES < <(basename -a "${UNITS[@]}")

sudo cp "${UNITS[@]}" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now "${UNIT_NAMES[@]}"

echo "설치 완료. 상태 확인:"
sudo systemctl status "${UNIT_NAMES[@]}" --no-pager -l | head -60
