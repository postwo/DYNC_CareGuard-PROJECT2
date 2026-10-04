#!/bin/bash
# careguard_turtlebot_agent 패키지를 로봇 라즈베리파이로 올리고 빌드한다.
#
# 사용법:
#   ./deploy_agent.sh <ssh계정@호스트> <원격 워크스페이스 경로>
#
# 예시:
#   ./deploy_agent.sh yeyu@100.110.210.121 '~/careguard_turtlebot_agent_ws'
#   (~는 따옴표로 감싸야 한다 — 안 감싸면 이 컴퓨터의 홈 경로로 먼저 바뀌어 로봇에 엉뚱한 경로가 넘어간다)
#
# 실행 전 준비물:
#   - 이 컴퓨터에서 그 로봇으로 SSH가 먼저 되어야 함(같은 공유기이거나 Tailscale로 연결됨)
#   - 로봇에 ROS2 humble, colcon, python3-pip가 이미 설치되어 있어야 함
set -euo pipefail

REMOTE="${1:?사용법: ./deploy_agent.sh <계정@호스트> <원격 워크스페이스 경로>}"
REMOTE_WS="${2:?사용법: ./deploy_agent.sh <계정@호스트> <원격 워크스페이스 경로>}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 경로는 원격 셸 명령과 rsync --delete 대상에 그대로 들어간다. 공백·특수문자가 섞이면 엉뚱한 곳을
# 만들거나 지우게 되므로 안전한 문자만 받는다.
if [[ ! "$REMOTE_WS" =~ ^[A-Za-z0-9_./~-]+$ ]]; then
  echo "에러: 원격 워크스페이스 경로에는 영문·숫자·_ . / ~ - 만 쓸 수 있습니다: $REMOTE_WS" >&2
  exit 1
fi

echo "[1/4] 로봇에 워크스페이스 디렉토리 생성"
ssh "$REMOTE" "mkdir -p $REMOTE_WS/src/careguard_turtlebot_agent"

echo "[2/4] 패키지 코드 rsync"
rsync -avz --delete \
  --exclude 'scripts' --exclude 'systemd' --exclude 'test' --exclude '*.md' --exclude '__pycache__' \
  "$SCRIPT_DIR/" "$REMOTE:$REMOTE_WS/src/careguard_turtlebot_agent/"

echo "[3/4] python 의존성 설치 (websockets)"
ssh "$REMOTE" "pip3 install --user websockets"

echo "[4/4] colcon build"
ssh "$REMOTE" "source /opt/ros/humble/setup.bash && cd $REMOTE_WS && colcon build --packages-select careguard_turtlebot_agent"

echo "완료: $REMOTE:$REMOTE_WS 에 빌드됨"
