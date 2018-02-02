#include "coroutine.h"
#include <stdio.h>

typedef struct Arg {
    Scheduler *s;
    int count;
} Arg;

void test(void *args) {
    Arg *arg = (Arg *)args;
    int i;
    for (i = 0; i < arg->count; ++i) {
        printf("[coroutine %d]: %d\n", coroutine_running(arg->s), i);
        coroutine_yield(arg->s);
    }
}

int main() {
    Scheduler *s = coroutine_open();
    Arg arg1 = {s, 10};
    Arg arg2 = {s, 8};
    int co1 = coroutine_create(s, test, (void *)&arg1);
    int co2 = coroutine_create(s, test, (void *)&arg2);
    printf("--start--\n");
    while (!coroutine_finished(s)) {
        coroutine_resume(s, co1);
        coroutine_resume(s, co2);
    }
    printf("--end--\n");
    coroutine_close(s);
}
