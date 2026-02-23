// AArch64 trampoline for launching a program in the shared address space.
//
// After clone3(), the child calls this to:
//   1. Redirect stdio file descriptors (dup3, raw syscalls)
//   2. Change working directory (chdir, raw syscall)
//   3. Close all inherited FDs > 2 (close_range)
//   4. Switch SP to the synthetic stack (argc/argv/envp/auxv)
//   5. Jump to ld-linux.so's _start
//
// No libc calls are made — the child's TLS is not yet initialized.

// Syscall numbers (AArch64 generic unistd)
.equ SYS_dup3,         24
.equ SYS_chdir,        49
.equ SYS_close,        57
.equ SYS_clone3,       435
.equ SYS_close_range,  436

.text
.global child_clone3_and_exec
.type child_clone3_and_exec, %function

// long child_clone3_and_exec(
//     struct clone_args *args,     // x0
//     size_t args_size,            // x1
//     void *synthetic_sp,          // x2
//     void *entry_point,           // x3 (ld-linux.so _start, or target _start)
//     int stdin_fd,                // x4
//     int stdout_fd,               // x5
//     int stderr_fd,               // x6
//     const char *cwd_path         // x7
// )
//
// Parent returns: child TID (positive) or -errno (negative).
// Child never returns.

child_clone3_and_exec:
	// Save callee-saved registers (parent needs them on return)
	stp	x29, x30, [sp, #-64]!
	mov	x29, sp
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]

	// Stash child-path arguments in callee-saved registers.
	// These survive the clone3 syscall unchanged in both parent and child.
	mov	x19, x2		// synthetic_sp
	mov	x20, x3		// entry_point
	mov	x21, x4		// stdin_fd
	mov	x22, x5		// stdout_fd
	mov	x23, x6		// stderr_fd
	mov	x24, x7		// cwd_path

	// clone3(args, args_size)
	// x0 = args, x1 = args_size  (already in place from caller)
	mov	x8, #SYS_clone3
	svc	#0

	// x0 = 0 in child, >0 (child TID) in parent, <0 on error
	cbz	x0, .Lchild

	// ===== PARENT / ERROR PATH =====
	// x0 = child TID or -errno — return it to caller
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x29, x30, [sp], #64
	ret

.Lchild:
	// ===== CHILD PATH =====
	// We are on the clone3-provided stack.
	// Registers x19-x24 hold our stashed arguments.
	// NO libc functions — raw syscalls only.

	// --- Redirect stdin ---
	cmp	w21, #0
	beq	1f
	mov	w0, w21
	mov	w1, #0		// STDIN_FILENO
	mov	w2, #0		// flags
	mov	x8, #SYS_dup3
	svc	#0
1:
	// --- Redirect stdout ---
	cmp	w22, #1
	beq	2f
	mov	w0, w22
	mov	w1, #1		// STDOUT_FILENO
	mov	w2, #0
	mov	x8, #SYS_dup3
	svc	#0
2:
	// --- Redirect stderr ---
	cmp	w23, #2
	beq	3f
	mov	w0, w23
	mov	w1, #2		// STDERR_FILENO
	mov	w2, #0
	mov	x8, #SYS_dup3
	svc	#0
3:
	// --- Close all inherited FDs above stderr ---
	mov	w0, #3		// first
	mov	x1, #-1		// last = UINT_MAX (close everything)
	mov	w2, #0		// flags
	mov	x8, #SYS_close_range
	svc	#0

	// --- Change working directory ---
	mov	x0, x24
	mov	x8, #SYS_chdir
	svc	#0

	// --- Switch to synthetic stack and launch ---
	mov	sp, x19		// synthetic_sp (points to argc)

	// Clear registers for a clean entry (mimics kernel ELF loader)
	mov	x0, #0
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x5, #0
	mov	x29, #0		// frame pointer
	mov	x30, #0		// link register

	br	x20			// jump to _start — never returns

.size child_clone3_and_exec, .-child_clone3_and_exec
