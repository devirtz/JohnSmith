option casemap:none

EXTERN IntelSetLaunchState:PROC
EXTERN IntelVmExitHandler:PROC

HV_MAGIC_RAX EQU 031504F5453564E45h
HV_MAGIC_RCX EQU 0C0DEC0DE4E41454Ch
HV_MAGIC_RDX EQU 053544F504F4E4C59h
HV_MAGIC_R8  EQU 0A55A5AA5F00DCAFEh

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

    mov rcx, rsp
    mov rdx, qword ptr [rsp + 120]
    sub rsp, 40
    call IntelVmExitHandler
    add rsp, 40
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

END
