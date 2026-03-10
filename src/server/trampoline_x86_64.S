// x86_64 trampoline for launching a program in the shared address space.
//
// After clone3(), the child calls this to:
//   1. Redirect stdio file descriptors (dup3, raw syscalls)
//   2. Close all inherited FDs > 2 (close_range)
//   3. Change working directory (chdir, raw syscall)
//   4. Set /proc/self/comm to basename(argv[0]) (prctl PR_SET_NAME)
//   5. Switch RSP to the synthetic stack (argc/argv/envp/auxv)
//   6. Jump to ld-linux.so's _start
//
// No libc calls are made — the child's TLS is not yet initialized.

// Syscall numbers (x86_64)
.equ SYS_close,        3
.equ SYS_chdir,        80
.equ SYS_prctl,        157
.equ SYS_dup3,         292
.equ SYS_clone3,       435
.equ SYS_close_range,  436

.equ PR_SET_NAME,      15

.text
.global child_clone3_and_exec
.type child_clone3_and_exec, @function

// long child_clone3_and_exec(
//     struct clone_args *args,     // rdi
//     size_t args_size,            // rsi
//     void *synthetic_sp,          // rdx
//     void *entry_point,           // rcx
//     int stdin_fd,                // r8
//     int stdout_fd,               // r9
//     int stderr_fd,               // 8(%rsp) at entry (7th arg)
//     const char *cwd_path         // 16(%rsp) at entry (8th arg)
// )
//
// Parent returns: child TID (positive) or -errno (negative).
// Child never returns.
//
// We need 6 values to survive clone3 (which clobbers rax, rcx, r11).
// Use all 6 callee-saved GPRs: rbx, rbp, r12, r13, r14, r15.
// No frame pointer — restore via rsp-relative offsets in parent path.
//
// Register allocation:
//   rbx  = synthetic_sp
//   rbp  = entry_point
//   r12d = stdin_fd
//   r13d = stdout_fd
//   r14d = stderr_fd
//   r15  = cwd_path

child_clone3_and_exec:
	// Save callee-saved registers
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	// Stack layout (rsp-relative):
	//   0(%rsp)   = saved r15
	//   8(%rsp)   = saved r14
	//   16(%rsp)  = saved r13
	//   24(%rsp)  = saved r12
	//   32(%rsp)  = saved rbx
	//   40(%rsp)  = saved rbp
	//   48(%rsp)  = return address
	//   56(%rsp)  = stderr_fd  (7th arg)
	//   64(%rsp)  = cwd_path   (8th arg)

	// Stash child-path arguments in callee-saved registers.
	// These survive the clone3 syscall unchanged in both parent and child.
	movq	%rdx, %rbx	// synthetic_sp
	movq	%rcx, %rbp	// entry_point
	movl	%r8d, %r12d	// stdin_fd
	movl	%r9d, %r13d	// stdout_fd
	movl	56(%rsp), %r14d	// stderr_fd
	movq	64(%rsp), %r15	// cwd_path

	// clone3(args, args_size)
	// rdi = args (already in place), rsi = args_size (already in place)
	movl	$SYS_clone3, %eax
	syscall

	// rax = 0 in child, >0 (child TID) in parent, <0 on error
	testq	%rax, %rax
	jz	.Lchild

	// ===== PARENT / ERROR PATH =====
	// rax = child TID or -errno — return it to caller
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret

.Lchild:
	// ===== CHILD PATH =====
	// We are on the clone3-provided stack.
	// Callee-saved registers hold our stashed arguments:
	//   rbx  = synthetic_sp
	//   rbp  = entry_point
	//   r12d = stdin_fd
	//   r13d = stdout_fd
	//   r14d = stderr_fd
	//   r15  = cwd_path
	// NO libc functions — raw syscalls only.
	// Note: syscall clobbers rcx and r11.

	// --- Redirect stdin ---
	cmpl	$0, %r12d
	je	1f
	movl	%r12d, %edi	// oldfd
	xorl	%esi, %esi	// newfd = STDIN_FILENO (0)
	xorl	%edx, %edx	// flags = 0
	movl	$SYS_dup3, %eax
	syscall
1:
	// --- Redirect stdout ---
	cmpl	$1, %r13d
	je	2f
	movl	%r13d, %edi	// oldfd
	movl	$1, %esi	// newfd = STDOUT_FILENO
	xorl	%edx, %edx	// flags = 0
	movl	$SYS_dup3, %eax
	syscall
2:
	// --- Redirect stderr ---
	cmpl	$2, %r14d
	je	3f
	movl	%r14d, %edi	// oldfd
	movl	$2, %esi	// newfd = STDERR_FILENO
	xorl	%edx, %edx	// flags = 0
	movl	$SYS_dup3, %eax
	syscall
3:
	// --- Close all inherited FDs above stderr ---
	movl	$3, %edi	// first = 3
	movl	$-1, %esi	// last = UINT_MAX (close everything)
	xorl	%edx, %edx	// flags = 0
	movl	$SYS_close_range, %eax
	syscall

	// --- Change working directory ---
	movq	%r15, %rdi	// cwd_path
	movl	$SYS_chdir, %eax
	syscall

	// --- Set /proc/self/comm to basename(argv[0]) ---
	// synthetic_sp is in rbx; [rbx + 8] is the argv[0] pointer.
	// Scan forward tracking the character after the last '/'.
	movq	8(%rbx), %rdi	// rdi = argv[0] string
	movq	%rdi, %rsi	// rsi = basename candidate (starts at argv[0])
.Lfind_basename:
	movzbl	(%rdi), %eax
	testl	%eax, %eax
	jz	.Lbasename_done
	cmpl	$47, %eax	// '/'
	jne	.Lnot_slash
	leaq	1(%rdi), %rsi	// update to char after '/'
.Lnot_slash:
	incq	%rdi
	jmp	.Lfind_basename
.Lbasename_done:
	movl	$PR_SET_NAME, %edi	// option
	// rsi already holds basename pointer (arg2)
	xorl	%edx, %edx	// arg3 = 0
	xorq	%r10, %r10	// arg4 = 0 (syscall convention uses r10 for 4th arg)
	xorq	%r8, %r8	// arg5 = 0
	movl	$SYS_prctl, %eax
	syscall			// ignore return value; best-effort

	// --- Switch to synthetic stack and launch ---
	movq	%rbx, %rsp	// synthetic_sp (points to argc)

	// Save entry point before clearing registers
	movq	%rbp, %r11	// r11 = entry_point (scratch register, fine to use here)

	// Clear registers for a clean entry (mimics kernel ELF loader)
	xorq	%rax, %rax
	xorq	%rbx, %rbx
	xorq	%rcx, %rcx
	xorq	%rdx, %rdx
	xorq	%rsi, %rsi
	xorq	%rdi, %rdi
	xorq	%r8, %r8
	xorq	%r9, %r9
	xorq	%r10, %r10
	xorq	%r12, %r12
	xorq	%r13, %r13
	xorq	%r14, %r14
	xorq	%r15, %r15
	xorq	%rbp, %rbp	// frame pointer

	jmpq	*%r11		// jump to _start — never returns

.size child_clone3_and_exec, .-child_clone3_and_exec
