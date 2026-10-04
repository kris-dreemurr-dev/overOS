#ifndef _SETJMP_H
#define _SETJMP_H

typedef struct {
    unsigned long regs[8]; // RBX, RBP, R12, R13, R14, R15, RSP, RIP
} jmp_buf[1];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif