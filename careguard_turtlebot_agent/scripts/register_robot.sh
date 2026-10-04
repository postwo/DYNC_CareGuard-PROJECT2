#!/bin/bash
# CareGuard 서버 DB에 새 로봇을 등록한다 (대시보드 "로봇 등록" 버튼과 동일한 API 호출).
#
# 사용법:
#   ./register_robot.sh <서버URL> <code> <name> <model> <ros_domain_id> [ip]
#
# 예시:
#   ./register_robot.sh http://100.107.185.97:8000 R-06 "R-06(yeyu)" turtlebot 40 100.110.210.121
#
# model은 "turtlebot" 또는 "limo" 등 서버가 아는 값이어야 한다.
# 이미 등록된 code면 서버가 409를 반환한다(에러 아님 — 이미 있다는 뜻).
set -euo pipefail

SERVER_URL="${1:?사용법: ./register_robot.sh <서버URL> <code> <name> <model> <ros_domain_id> [ip]}"
CODE="${2:?}"
NAME="${3:?}"
MODEL="${4:?}"
DOMAIN="${5:?}"
IP="${6:-}"

# 서버는 ros_domain_id를 정수로 받는다(0~232). 문자열을 그대로 JSON에 넣으면 서버가 422를 돌려준다.
if ! [[ "$DOMAIN" =~ ^[0-9]+$ ]] || [ "$DOMAIN" -gt 232 ]; then
  echo "에러: ros_domain_id는 0~232 사이 정수여야 합니다: $DOMAIN" >&2
  exit 1
fi

# JSON은 문자열을 이어 붙이지 않고 python으로 만든다 — 이름에 따옴표나 역슬래시가 있어도 깨지지 않는다.
BODY="$(python3 -c '
import json, sys
code, name, model, domain, ip = sys.argv[1:6]
print(json.dumps({"code": code, "name": name, "model": model, "ros_domain_id": int(domain), "ip": ip or None}))
' "$CODE" "$NAME" "$MODEL" "$DOMAIN" "$IP")"

# -f를 쓰지 않고 상태 코드를 직접 본다 — -f는 4xx에서 서버가 보낸 사유(본문)를 버리고 조용히 실패한다.
RESPONSE_FILE="$(mktemp)"
trap 'rm -f "$RESPONSE_FILE"' EXIT
STATUS="$(curl -s -o "$RESPONSE_FILE" -w '%{http_code}' -X POST "$SERVER_URL/robots" \
  -H "Content-Type: application/json" -d "$BODY" || true)"

case "$STATUS" in
  200|201)
    python3 -m json.tool < "$RESPONSE_FILE"
    ;;
  409)
    echo "이미 등록된 로봇 코드입니다($CODE) — 그대로 사용하면 됩니다." >&2
    ;;
  000)
    echo "에러: 서버에 연결할 수 없습니다: $SERVER_URL" >&2
    exit 1
    ;;
  *)
    echo "에러: 등록 실패 (HTTP $STATUS)" >&2
    cat "$RESPONSE_FILE" >&2
    echo >&2
    exit 1
    ;;
esac
