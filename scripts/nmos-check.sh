#!/bin/bash
# Runs AMWA's NMOS test suites (nmos-testing, Apache-2.0) against Montage's NMOS Node: IS-04's Node API and
# registration (IS-04-01), IS-05's Connection API (IS-05-01) and the two together (IS-05-02), with the registry found by
# unicast DNS-SD from the test tool's own DNS server. The Node is `montage-cli st2110`, sending colour bars and tone.
#
#   sudo scripts/nmos-check.sh build/src/montage-cli <work directory>
#
# Root, as the tool's DNS server listens on port 53 of this computer's address. Exits non-zero when a test fails.
set -euo pipefail
cli=$(realpath "$1")
work=$(realpath -m "$2")
ref=dcd63b03fa4f107803c00c60f4408feb10744849  # (nmos-testing, 22 October 2026)
mkdir -p "$work"
if [ ! -d "$work/nmos-testing" ]; then
    git clone -q https://github.com/AMWA-TV/nmos-testing.git "$work/nmos-testing"
    git -C "$work/nmos-testing" checkout -q "$ref"
fi
[ -x "$work/venv/bin/python" ] || python3 -m venv "$work/venv"
"$work/venv/bin/pip" install -q -r "$work/nmos-testing/requirements.txt"
printf "from . import Config as CONFIG\nCONFIG.DNS_SD_MODE = 'unicast'\n" > "$work/nmos-testing/nmostesting/UserConfig.py"

# The address the tool's DNS server and mock registries use: the default route's interface's.
host=$("$work/venv/bin/python" -I -c "import netifaces as n; i = n.gateways()['default'][n.AF_INET][1]; print(n.ifaddresses(i)[n.AF_INET][0]['addr'])")
QT_QPA_PLATFORM=offscreen "$cli" st2110 --mode 720p50 --interface "$host" --nmos-port 8080 --domain testsuite.nmos.tv \
    --dns-server "$host" --seconds 3600 > "$work/node.log" 2>&1 &
node=$!
trap 'kill $node 2> /dev/null || true' EXIT
sleep 2

failed=0
run() {
    local suite=$1
    shift
    (cd "$work/nmos-testing" && timeout 900 "$work/venv/bin/python" nmos-test.py suite "$suite" "$@" --output "$work/$suite.xml" \
        > "$work/$suite.log" 2>&1) || true
    # Passed, or not applicable (no receivers, no authorisation): anything failing or in error is reported.
    "$work/venv/bin/python" -I - "$work/$suite.xml" "$suite" <<'EOF' || failed=1
import sys
import xml.etree.ElementTree as ET
path, suite = sys.argv[1], sys.argv[2]
passed = skipped = 0
bad = []
for case in ET.parse(path).iter("testcase"):
    problem = case.find("failure") if case.find("failure") is not None else case.find("error")
    if problem is not None:
        bad.append("{}: {}".format(case.get("name"), problem.get("message") or problem.text or ""))
    elif case.find("skipped") is not None:
        skipped += 1
    else:
        passed += 1
print("{}: {} passed, {} not applicable, {} failed".format(suite, passed, skipped, len(bad)))
for b in bad:
    print("  " + b)
sys.exit(1 if bad or passed == 0 else 0)
EOF
}
run IS-04-01 --host "$host" --port 8080 --version v1.3
run IS-05-01 --host "$host" --port 8080 --version v1.1
run IS-05-02 --host "$host" "$host" --port 8080 8080 --version v1.3 v1.1
if [ "$failed" -ne 0 ]; then
    echo "The Node's log:"
    cat "$work/node.log"
fi
exit "$failed"
