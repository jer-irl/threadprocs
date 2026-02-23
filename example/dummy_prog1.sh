#!/usr/bin/env bash

set -o errexit
set -o pipefail
set -o xtrace

testpath="buildout"
dummy_sock="${testpath}/dummy.sock"
server_sock="${testpath}/server.sock"
rm -f "${dummy_sock}" "${server_sock}"

server_cmd="${testpath}/server ${server_sock}"
launcher_cmd="${testpath}/launcher ${server_sock}"

cleanup() {
	set +e
	if [[ -n "${dummy_server_pid:-}" ]]; then
		kill -TERM "${dummy_server_pid}" 2>/dev/null || true
		wait "${dummy_server_pid}" 2>/dev/null || true
	fi
	if [[ -n "${server_pid:-}" ]]; then
		kill -TERM "${server_pid}" 2>/dev/null || true
		wait "${server_pid}" 2>/dev/null || true
	fi
}

trap cleanup EXIT

echo "Launching server..."
${server_cmd} &
server_pid=$!
sleep 1 # Give the server a moment to start up

echo "Launching dummy server..."
${launcher_cmd} ${testpath}/dummy_server ${dummy_sock} &
dummy_server_pid=$!
sleep 1 # Give the dummy server a moment to start up

echo "Launching dummy client..."
${launcher_cmd} ${testpath}/dummy_client ${dummy_sock}
