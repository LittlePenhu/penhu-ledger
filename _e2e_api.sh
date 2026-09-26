#!/usr/bin/env bash
# =============================================================================
#  端到端 HTTP 验证：起服务，用 curl 打真实接口，把结果打印出来
#
#  为什么要有这一步：单元测试是我自己写的、自己调用的，它能证明「函数返回值
#  符合我的预期」，但不能证明「HTTP 层的路由、鉴权、序列化串起来是通的」。
#  这一步的每一条都是**真实进程 + 真实 TCP + 真实 JSON**，
#  而且可以被任何人原样复现（脚本就在仓库里）。
#
#  用法: bash _e2e_api.sh
# =============================================================================
set -uo pipefail

PORT="${PORT:-18111}"
PROJ="$(cd "$(dirname "$0")" && pwd)"
DATA_WIN="$(cd "$PROJ" && pwd -W)\\_e2e_data"
BASE="http://127.0.0.1:${PORT}"

# ---- 起服务（脚本自足：不用先手动起 penhu-cli serve）----
# 用独立数据目录，跑完就删：端到端测试注册的账号不该攒在开发数据里。
E2E_DATA_BASH="$PROJ/_e2e_data"
rm -rf "$E2E_DATA_BASH"
"$PROJ/build/bin/penhu-cli.exe" serve --data-dir "$DATA_WIN" --port "$PORT" >/dev/null 2>&1 &
SRV_PID=$!
trap 'kill $SRV_PID 2>/dev/null; rm -rf "$E2E_DATA_BASH"' EXIT
for _ in $(seq 1 40); do
  curl -sS -m 2 "$BASE/api/health" >/dev/null 2>&1 && break
  sleep 0.5
done

PASS=0
FAIL=0
ok()   { PASS=$((PASS+1)); printf '  [OK]   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  [FAIL] %s\n' "$1"; }
# 断言 JSON 里某个字段等于期望值
expect() { # expect <json> <jq-less python 表达式> <说明>
  local json="$1" expr="$2" desc="$3"
  local got
  got="$(printf '%s' "$json" | python -c "
import sys, json
try:
    d = json.load(sys.stdin)
except Exception as e:
    print('JSON解析失败:', e); raise SystemExit
print(eval(\"$expr\"))
" 2>&1 | tail -1)"
  if [ "$got" = "True" ]; then ok "$desc"; else bad "$desc  (实得: $got)"; fi
}

curl_json() { curl -sS -m 15 "$@" 2>/dev/null; }

echo "=============================================="
echo " PenHu Ledger 端到端 HTTP 验证"
echo " 服务地址: $BASE   数据目录: $DATA_WIN"
echo "=============================================="

# ---- 1) 健康检查（唯一不需要鉴权的业务接口）----
echo ""
echo "=== 1) 健康检查 ==="
H="$(curl_json "$BASE/api/health")"
echo "        响应: $H"
expect "$H" "d['ok'] is True" "返回 ok=true"
expect "$H" "d['data']['version']=='0.1.0'" "版本号是 0.1.0"

# ---- 2) 不再提供网页（WebView 方案已废弃）----
echo ""
echo "=== 2) 非 /api/ 路径不应返回网页 ==="
# 界面改成原生自绘（native/，Win32 + Direct2D + DirectWrite）之后，
# 这个服务只提供 /api/ 下的 JSON 接口，没有 HTML/CSS/JS 可发。
# 这几条断言是**反着**钉的：确认它不会再吐出网页 ——
# 否则将来有人顺手把静态资源加回来，就等于把已经拆掉的 WebView 方案又接上了。
for path in / /index.html /styles.css /app.js; do
  code="$(curl -sS -m 15 -o /dev/null -w '%{http_code} %{content_type}' "$BASE$path" 2>/dev/null)"
  if printf '%s' "$code" | grep -q "^404 " && ! printf '%s' "$code" | grep -q "text/html"; then
    ok "GET $path  -> $code （按预期不再是网页）"
  else
    bad "GET $path  -> $code （期望 404 且非 text/html）"
  fi
done

# ---- 3) Host 头校验（挡 DNS rebinding）----
echo ""
echo "=== 3) Host 头校验（防 DNS rebinding）==="
# 用一个「看起来不像回环地址」的 Host 去打本机端口，必须被拒
SNEAKY="$(curl_json -H 'Host: evil.example.com' "$BASE/api/health")"
echo "        响应: $SNEAKY"
if printf '%s' "$SNEAKY" | grep -q '"ok":false'; then
  ok "伪造 Host 被拒（这是挡 DNS rebinding 的关键一环）"
else
  bad "伪造 Host 没被拒！"
fi

# ---- 4) 注册 / 登录 / 鉴权 ----
echo ""
echo "=== 4) 注册、登录与鉴权 ==="
U="e2euser_$$"
P="e2e-Pass-2026"
R="$(curl_json -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
      -d "{\"username\":\"$U\",\"password\":\"$P\"}")"
echo "        注册响应: $R"
expect "$R" "d['ok'] is True" "注册成功"

# 未带 token 访问受保护接口必须 401
NOKEY="$(curl -sS -m 15 -o /dev/null -w '%{http_code}' "$BASE/api/records" 2>/dev/null)"
[ "$NOKEY" = "401" ] && ok "无 token 访问 /api/records -> 401" || bad "无 token 访问 /api/records -> $NOKEY（期望 401）"

L="$(curl_json -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
      -d "{\"username\":\"$U\",\"password\":\"$P\"}")"
TOKEN="$(printf '%s' "$L" | python -c "import sys,json;print(json.load(sys.stdin)['data']['token'])" 2>/dev/null)"
if [ -n "$TOKEN" ]; then ok "登录拿到 token（长度 ${#TOKEN}）"; else bad "登录没拿到 token: $L"; fi

WRONGPW="$(curl_json -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
            -d "{\"username\":\"$U\",\"password\":\"definitely-wrong\"}")"
expect "$WRONGPW" "d['ok'] is False" "错误密码被拒"

AUTH=(-H "Authorization: Bearer $TOKEN")

# ---- 5) 记账 ----
echo ""
echo "=== 5) 记账（含跨天、多分类）==="
CATS="$(curl_json "${AUTH[@]}" "$BASE/api/categories")"
CAT_ID="$(printf '%s' "$CATS" | python -c "
import sys,json
d=json.load(sys.stdin)['data']
exp=[c for c in d if c['direction']=='expense']
print(exp[0]['id'] if exp else '')" 2>/dev/null)"
if [ -n "$CAT_ID" ]; then ok "取到支出分类（id=${CAT_ID:0:8}…）"; else bad "取不到分类: $CATS"; fi

ADDED=0
for i in 1 2 3 4 5; do
  AMT=$((i * 1234))
  # 请求体走临时文件而不是 -d 参数：Git Bash 调 Windows curl.exe 时 argv 会被
  # 转成 ANSI(GBK)，中文 note 到服务端就成了非法 UTF-8（json 直接拒收）。
  # 文件字节不经过 argv 编码转换，原样送达。
  BODY_FILE="$(mktemp)"
  printf '{"amountMinor":%d,"direction":"expense","categoryId":"%s","date":"2026-09-2%s","note":"E2E 第 %s 笔"}' \
    "$AMT" "$CAT_ID" "$i" "$i" > "$BODY_FILE"
  ONE="$(curl_json -X POST "$BASE/api/records" "${AUTH[@]}" -H 'Content-Type: application/json' -d "@$BODY_FILE")"
  rm -f "$BODY_FILE"
  if printf '%s' "$ONE" | grep -q '"ok":true'; then ADDED=$((ADDED+1)); fi
done
[ "$ADDED" = "5" ] && ok "写入 5 笔记录" || bad "只写入 $ADDED 笔（末笔响应: $ONE）"

# 方向与分类不匹配必须被拒（支出用「工资」这种收入分类）
INC_CAT="$(printf '%s' "$CATS" | python -c "
import sys,json
d=json.load(sys.stdin)['data']
inc=[c for c in d if c['direction']=='income']
print(inc[0]['id'] if inc else '')" 2>/dev/null)"
if [ -n "$INC_CAT" ]; then
  MISMATCH="$(curl_json -X POST "$BASE/api/records" "${AUTH[@]}" -H 'Content-Type: application/json' \
    -d "{\"amountMinor\":100,\"direction\":\"expense\",\"categoryId\":\"$INC_CAT\",\"date\":\"2026-09-20\"}")"
  expect "$MISMATCH" "d['ok'] is False" "支出 + 收入分类 被拒"
fi

# 金额为 0 必须被拒
ZERO="$(curl_json -X POST "$BASE/api/records" "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d "{\"amountMinor\":0,\"direction\":\"expense\",\"categoryId\":\"$CAT_ID\",\"date\":\"2026-09-20\"}")"
expect "$ZERO" "d['ok'] is False" "金额 0 被拒"

# ---- 6) 列表与统计 ----
echo ""
echo "=== 6) 列表与统计 ==="
RECS="$(curl_json "${AUTH[@]}" "$BASE/api/records?from=2026-09-01&to=2026-09-30")"
expect "$RECS" "d['ok'] is True and len(d['data']['items'])==5" "列表返回 5 笔"
# 用金额**集合**断言，不要按位置断言：列表按日期倒序，位置会随排序规则变化而变，
# 按位置写的断言迟早会因为「排序改了」而假失败。
expect "$RECS" "sorted(x['amountMinor'] for x in d['data']['items'])==[1234,2468,3702,4936,6170]" \
       "5 笔金额为 1234/2468/3702/4936/6170 分"

SUM="$(curl_json "${AUTH[@]}" "$BASE/api/analytics?from=2026-09-01&to=2026-09-30&trendWindow=7")"
echo "        统计响应片段: $(printf '%s' "$SUM" | head -c 220)…"
# 1+2+3+4+5 = 15 → 15*1234 = 18510 分
expect "$SUM" "d['data']['summary']['expenseMinor']==18510" "区间支出合计 18510 分（=¥185.10）"
expect "$SUM" "len(d['data']['trend']['points'])==30" "趋势序列补齐 30 天"
expect "$SUM" "d['data']['summary']['expenseRecords']==5" "支出笔数 5"

# ---- 7) 报告（无模型通道 -> 模板兜底，必须不是报错也不是空白）----
echo ""
echo "=== 7) 报告生成（走模板兜底）==="
REP="$(curl_json -X POST "$BASE/api/reports/generate" "${AUTH[@]}" -H 'Content-Type: application/json' \
        -d '{"kind":"monthly","anchor":"2026-09-20"}')"
expect "$REP" "d['ok'] is True" "生成报告成功"
expect "$REP" "len(d['data']['content'])>50" "报告正文非空（>50 字）"
PROVIDER="$(printf '%s' "$REP" | python -c "
import sys,json
print(json.load(sys.stdin)['data'].get('provider',''))" 2>/dev/null)"
[ -n "$PROVIDER" ] && ok "报告通道标记: $PROVIDER（两条通道都没配时应当是 rule-based）" || bad "报告没标通道"

# ---- 8) 越权：换一个账号去读别人的记录 ----
echo ""
echo "=== 8) 跨账号越权 ==="
U2="e2ebob_$$"
curl_json -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
  -d "{\"username\":\"$U2\",\"password\":\"$P\"}" > /dev/null
L2="$(curl_json -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
       -d "{\"username\":\"$U2\",\"password\":\"$P\"}")"
T2="$(printf '%s' "$L2" | python -c "import sys,json;print(json.load(sys.stdin)['data']['token'])" 2>/dev/null)"
RECS2="$(curl_json -H "Authorization: Bearer $T2" "$BASE/api/records?from=2026-09-01&to=2026-09-30")"
expect "$RECS2" "len(d['data']['items'])==0" "另一个账号看不到这些记录（账本隔离）"

echo ""
echo "=============================================="
echo " 通过 $PASS 项，失败 $FAIL 项"
echo "=============================================="
[ "$FAIL" -eq 0 ] || exit 1
