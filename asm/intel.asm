option casemap:none

EXTERN IntelSetLaunchState:PROC
EXTERN IntelVmExitHandler:PROC

HV_MAGIC_RAX EQU 031504F5453564E45h
HV_MAGIC_RCX EQU 0C0DEC0DE4E41454Ch
HV_MAGIC_RDX EQU 053544F504F4E4C59h
HV_MAGIC_R8  EQU 0A55A5AA5F00DCAFEh

VMCS_EXIT_REASON              EQU 04402h
VMCS_EXIT_INSTRUCTION_LENGTH  EQU 0440Ch
VMCS_GUEST_RIP                EQU 0681Eh

VMX_EXIT_CPUID                EQU 10
VMX_EXIT_VMCALL               EQU 18

HOST_FRAME_FAST_PATH_ENABLED  EQU 40
HOST_FRAME_RENDEZVOUS_PHASE   EQU 48
INTEL_RENDEZVOUS_IDLE         EQU 0

; INTEL_CPU_CONTEXT layout consumed by this file.  Compile-time asserts in
; include/intel.h keep the offsets in sync.
HV_CPU_VENDOR_CONTEXT         EQU 16
CPU_CONTEXT_GUEST_CR2         EQU 72

IFNDEF JOHNSMITH_VMEXIT_BENCHMARK
JOHNSMITH_VMEXIT_BENCHMARK EQU 0
ENDIF

.code

IntelAsmLaunch PROC
    mov rcx, rsp
    lea rdx, IntelGuestResume
    sub rsp, 40
    call IntelSetLaunchState
    add rsp, 40
    test eax, eax
    jnz IntelLaunchFailed
    vmlaunch
IntelLaunchFailed:
    mov eax, 1
    ret

IntelGuestResume:
    xor eax, eax
    ret
IntelAsmLaunch ENDP

IntelAsmStop PROC
    mov r9, rcx
    mov rax, HV_MAGIC_RAX
    mov rcx, HV_MAGIC_RCX
    mov rdx, HV_MAGIC_RDX
    mov r8,  HV_MAGIC_R8
    vmcall
    ret
IntelAsmStop ENDP

IntelAsmInvept PROC
    invept rcx, oword ptr [rdx]
    jz IntelInveptFailed
    jc IntelInveptFailed
    xor eax, eax
    ret
IntelInveptFailed:
    mov eax, 1
    ret
IntelAsmInvept ENDP

IntelAsmInvvpid PROC
    invvpid rcx, oword ptr [rdx]
    jz IntelInvvpidFailed
    jc IntelInvvpidFailed
    xor eax, eax
    ret
IntelInvvpidFailed:
    mov eax, 1
    ret
IntelAsmInvvpid ENDP

IntelAsmVmExit PROC
    ; Handle the benchmark VMCALL without constructing a C ABI frame.
    push r8
    push r9
    push r10
    push r11

    lea r10, [rsp + 32]
    cmp qword ptr [r10 + HOST_FRAME_FAST_PATH_ENABLED], 1
    jne IntelVmExitSlowPath

    mov r8d, VMCS_EXIT_REASON
    vmread r9, r8
    jbe IntelVmExitSlowPath
    cmp r9d, VMX_EXIT_CPUID
    je IntelVmExitSlowPath

IF JOHNSMITH_VMEXIT_BENCHMARK
    cmp r9d, VMX_EXIT_VMCALL
    jne IntelVmExitSlowPath
    mov r8, 04A534D5642454E43h
    cmp rax, r8
    jne IntelVmExitSlowPath
    mov r8, 0484D41524B464C52h
    cmp rcx, r8
    jne IntelVmExitSlowPath
    mov r8, 0564D43414C4C3031h
    cmp rdx, r8
    jne IntelVmExitSlowPath
    mov r8, 0B16B00B5DEADC0DEh
    cmp qword ptr [rsp + 24], r8
    jne IntelVmExitSlowPath
    mov r8, qword ptr [r10 + HOST_FRAME_RENDEZVOUS_PHASE]
    test r8, r8
    jz IntelVmExitSlowPath
    cmp dword ptr [r8], INTEL_RENDEZVOUS_IDLE
    jne IntelVmExitSlowPath
    ; Deliberately minimal, benchmark-only VMCALL completion.  This measures
    ; VM-exit + RIP advancement + VMRESUME without entering C.
    jmp IntelVmExitBenchmarkAdvanceRip
ELSE
    jmp IntelVmExitSlowPath
ENDIF

IF JOHNSMITH_VMEXIT_BENCHMARK
IntelVmExitBenchmarkAdvanceRip:
    mov r8d, VMCS_EXIT_INSTRUCTION_LENGTH
    vmread r11, r8
    jbe IntelVmExitSlowPath
    mov r8d, VMCS_GUEST_RIP
    vmread r9, r8
    jbe IntelVmExitSlowPath
    add r9, r11
    vmwrite r9, r8
    jbe IntelVmExitSlowPath
    jmp IntelVmExitFastResume
ENDIF

IntelVmExitFastResume:
    pop r11
    pop r10
    pop r9
    pop r8
    vmresume
    ud2

IntelVmExitSlowPath:
    pop r11
    pop r10
    pop r9
    pop r8

    ; Snapshot guest CR2 before any host code path can fault and clobber it.
    ; VMX does not save or restore CR2, so host CR2 == guest CR2 on VM exit
    ; and stays that way until we run something that faults.  Fast path never
    ; touches CR2 and skips this save/restore.
    push rax
    push rcx
    mov rcx, qword ptr [rsp + 16]                ; host frame: HV_CPU*
    mov rcx, qword ptr [rcx + HV_CPU_VENDOR_CONTEXT]
    mov rax, cr2
    mov qword ptr [rcx + CPU_CONTEXT_GUEST_CR2], rax
    pop rcx
    pop rax

    ; Ascending frame offsets are RAX, RCX, RDX, RBX, RBP, RSI, RDI,
    ; R8..R15.  VMCS host RSP points at the owning HV_CPU pointer.
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax

    ; The Windows x64 ABI lets C clobber XMM0-XMM5, but VM exits must be
    ; transparent to the interrupted guest context.
    sub rsp, 96
    movdqu xmmword ptr [rsp + 0], xmm0
    movdqu xmmword ptr [rsp + 16], xmm1
    movdqu xmmword ptr [rsp + 32], xmm2
    movdqu xmmword ptr [rsp + 48], xmm3
    movdqu xmmword ptr [rsp + 64], xmm4
    movdqu xmmword ptr [rsp + 80], xmm5

    lea rcx, [rsp + 96]
    mov rdx, qword ptr [rsp + 216]
    sub rsp, 40
    call IntelVmExitHandler
    add rsp, 40

    movdqu xmm0, xmmword ptr [rsp + 0]
    movdqu xmm1, xmmword ptr [rsp + 16]
    movdqu xmm2, xmmword ptr [rsp + 32]
    movdqu xmm3, xmmword ptr [rsp + 48]
    movdqu xmm4, xmmword ptr [rsp + 64]
    movdqu xmm5, xmmword ptr [rsp + 80]
    add rsp, 96

    ; Restore guest CR2.  Handler may have overwritten context->GuestCr2 to
    ; inject a #PF; if not, the value flows back unchanged.  Both the
    ; resume and shutdown exits go through here.
    mov r10, qword ptr [rsp + 120]               ; host frame: HV_CPU*
    mov r10, qword ptr [r10 + HV_CPU_VENDOR_CONTEXT]
    mov r10, qword ptr [r10 + CPU_CONTEXT_GUEST_CR2]
    mov cr2, r10

    cmp eax, 1
    je IntelShutdown

    pop rax
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    vmresume
    ud2

IntelShutdown:
    mov rdx, qword ptr [rsp + 120]
    mov rdx, qword ptr [rdx + 16]
    mov r10, qword ptr [rdx + 56]
    mov r11, qword ptr [rdx + 64]
    vmxoff

    mov rbx, qword ptr [rsp + 24]
    mov rbp, qword ptr [rsp + 32]
    mov rsi, qword ptr [rsp + 40]
    mov rdi, qword ptr [rsp + 48]
    mov r12, qword ptr [rsp + 88]
    mov r13, qword ptr [rsp + 96]
    mov r14, qword ptr [rsp + 104]
    mov r15, qword ptr [rsp + 112]
    mov qword ptr [r10 - 8], r11
    lea rsp, [r10 - 8]
    ret
IntelAsmVmExit ENDP

IntelAsmReadEs PROC
    xor eax, eax
    mov ax, es
    ret
IntelAsmReadEs ENDP

IntelAsmReadCs PROC
    xor eax, eax
    mov ax, cs
    ret
IntelAsmReadCs ENDP

IntelAsmReadSs PROC
    xor eax, eax
    mov ax, ss
    ret
IntelAsmReadSs ENDP

IntelAsmReadDs PROC
    xor eax, eax
    mov ax, ds
    ret
IntelAsmReadDs ENDP

IntelAsmReadFs PROC
    xor eax, eax
    mov ax, fs
    ret
IntelAsmReadFs ENDP

IntelAsmReadGs PROC
    xor eax, eax
    mov ax, gs
    ret
IntelAsmReadGs ENDP

IntelAsmReadLdtr PROC
    xor eax, eax
    sldt ax
    ret
IntelAsmReadLdtr ENDP

IntelAsmReadTr PROC
    xor eax, eax
    str ax
    ret
IntelAsmReadTr ENDP

IntelAsmStoreGdtr PROC
    sgdt fword ptr [rcx]
    ret
IntelAsmStoreGdtr ENDP

IntelAsmStoreIdtr PROC
    sidt fword ptr [rcx]
    ret
IntelAsmStoreIdtr ENDP

IntelAsmLoadGdtr PROC
    lgdt fword ptr [rcx]
    ret
IntelAsmLoadGdtr ENDP

IntelAsmLoadIdtr PROC
    lidt fword ptr [rcx]
    ret
IntelAsmLoadIdtr ENDP

END
