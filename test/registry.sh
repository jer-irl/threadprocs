#!/usr/bin/env bash

set -o errexit

testpath="buildout"
server_sock="${testpath}/examples/registry.sock"
rm -f "${server_sock}"

server_cmd="${testpath}/server ${server_sock}"
launcher_cmd="${testpath}/launcher ${server_sock}"

tmpdir=$(mktemp -d)

cleanup() {
	set +e
	if [[ -n "${server_pid:-}" ]]; then
		kill -TERM "${server_pid}" 2>/dev/null || true
		wait "${server_pid}" 2>/dev/null || true
	fi
	rm -rf "${tmpdir}"
}
trap cleanup EXIT

echo "=== Test: threadproc registry detection ==="

echo "Launching server..."
${server_cmd} &
server_pid=$!
sleep 1

# Test 1: run detect as a threadproc
echo "Launching detect as threadproc..."
${launcher_cmd} ${testpath}/examples/detect \
	> "${tmpdir}/tp_out" 2>"${tmpdir}/tp_err"

echo "threadproc output:"
cat "${tmpdir}/tp_out"
echo "---"

grep -q "^threadproc: yes$" "${tmpdir}/tp_out" \
	|| { echo "FAIL: expected 'threadproc: yes'"; exit 1; }

grep -q "^registry: 0x" "${tmpdir}/tp_out" \
	|| { echo "FAIL: expected non-null registry pointer"; exit 1; }

grep -q "^wrote:" "${tmpdir}/tp_out" \
	|| { echo "FAIL: expected successful write to registry page"; exit 1; }

echo "PASS: threadproc detection correct"

# Test 2: run detect directly (not as threadproc)
echo "Launching detect directly..."
${testpath}/examples/detect > "${tmpdir}/direct_out" 2>"${tmpdir}/direct_err" || true

echo "direct output:"
cat "${tmpdir}/direct_out"
echo "---"

grep -q "^threadproc: no$" "${tmpdir}/direct_out" \
	|| { echo "FAIL: expected 'threadproc: no' when run directly"; exit 1; }

echo "PASS: non-threadproc detection correct"
echo "=== All registry tests passed ==="
