#!/usr/bin/env bash

set -o errexit

testpath="buildout"
server_sock="${testpath}/examples/stringshare.sock"
rm -f "${server_sock}"

server_cmd="${testpath}/server ${server_sock}"
launcher_cmd="${testpath}/launcher ${server_sock}"

tmpdir=$(mktemp -d)

cleanup() {
	set +e
	if [[ -n "${allocstr_pid:-}" ]]; then
		kill "${allocstr_pid}" 2>/dev/null || true
		wait "${allocstr_pid}" 2>/dev/null || true
	fi
	if [[ -n "${server_pid:-}" ]]; then
		kill -TERM "${server_pid}" 2>/dev/null || true
		wait "${server_pid}" 2>/dev/null || true
	fi
	rm -rf "${tmpdir}"
}

trap cleanup EXIT

test_string="hello from threadprocs"

echo "Launching server..."
${server_cmd} &
server_pid=$!
sleep 1

# Launch allocstr in the shared address space.
# The subshell delays the write so the pipe's write end stays open until
# allocstr is alive and reading. Without this, echo closes the pipe
# before the launcher→server→clone3→dup3 chain finishes, and getline
# sees immediate EOF.
echo "Launching allocstr..."
(sleep 3; echo "${test_string}") | ${launcher_cmd} ${testpath}/examples/allocstr \
	> "${tmpdir}/allocstr_out" 2>"${tmpdir}/allocstr_err" &
allocstr_pid=$!
sleep 5

# Extract the hex address of the allocated string
echo "Parsing allocstr output..."
echo "allocstr output (first 10 lines):"
head -n 10 "${tmpdir}/allocstr_out" || true
echo "---"
address=$(grep -oEm1 '0x[0-9a-fA-F]+' "${tmpdir}/allocstr_out" || true)
if [[ -z "${address}" ]]; then
	echo "FAIL: Could not find address in allocstr output"
	echo "allocstr stderr:"
	head -n 20 "${tmpdir}/allocstr_err" || true
	exit 1
fi
echo "Allocated string at: ${address}"

# Launch printstr in the same shared address space. It dereferences the
# pointer directly from allocstr's heap. Same delayed-write approach.
# After printing the string it will loop back, read EOF, and crash on
# a nullptr dereference, so we tolerate a non-zero exit.
echo "Launching printstr..."
(sleep 3; echo "${address}") | ${launcher_cmd} ${testpath}/examples/printstr \
	> "${tmpdir}/printstr_out" 2>"${tmpdir}/printstr_err" || true

# Extract the string content from printstr output.
# Output line: "The string content at address 0x... is: <string>"
echo "Parsing printstr output..."
echo "printstr output (first 10 lines):"
head -n 10 "${tmpdir}/printstr_out" || true
echo "---"
result=$(grep -m1 "is:" "${tmpdir}/printstr_out" | sed 's/.*is: //' || true)
echo "Printstr read: '${result}'"

if [[ "${result}" == "${test_string}" ]]; then
	echo "PASS: String successfully shared across threadprocs"
	exit 0
else
	echo "FAIL: Expected '${test_string}', got '${result}'"
	echo "printstr stderr:"
	head -n 20 "${tmpdir}/printstr_err" || true
	exit 1
fi
