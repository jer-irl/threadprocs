#!/usr/bin/env bash

set -o errexit
set -o pipefail
set -o xtrace

testpath="buildout"
dummy_sock="${testpath}/dummy.sock"
server_sock="${testpath}/server.sock"
rm -f "${dummy_sock}" "${server_sock}"

dummy_server_cmd="${testpath}/dummy_prog1 server ${dummy_sock}"
dummy_client_cmd="${testpath}/dummy_prog1 client ${dummy_sock}"

server_cmd="${testpath}/server ${server_sock}"
launcher_cmd="${testpath}/launcher ${server_sock}"

echo "Launching server..."
${server_cmd} &
server_pid=$!
trap "kill -TERM ${server_pid} 2>/dev/null || true; wait ${server_pid}" EXIT
sleep 1 # Give the server a moment to start up

echo "Launching dummy server..."
${launcher_cmd} ${dummy_server_cmd} &
dummy_server_pid=$!
trap "kill -TERM ${dummy_server_pid} 2>/dev/null || true; wait ${dummy_server_pid}" EXIT
sleep 1 # Give the dummy server a moment to start up

echo "Launching dummy client..."
${launcher_cmd} ${dummy_client_cmd}

kill -TERM ${dummy_server_pid} 2>/dev/null || true
wait ${dummy_server_pid}
