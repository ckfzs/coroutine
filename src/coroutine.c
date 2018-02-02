#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>
#include <assert.h>

#define DEFAULT_STACK_SZIE 1024 * 1024
#define DEFAULT_COROUTINE_NUM 16

enum CoState {                // 协程状态
    CO_READY = 0,             // 刚创建完毕
    CO_RUNNING,               // 正在运行中
    CO_SUSPEND,               // 被挂起
    CO_DONE                   // 运行结束
};

struct Coroutine {            // 协程结构体
    Scheduler *scher;         // 从属的协程调度器
    CoFunc func;              // 函数入口
    void *arg;                // 函数参数
    CoState state;            // 协程当前状态
    ucontext_t ctx;           // 协程上下文

    int size;                 // 协程栈大小
    char *stack;              // 协程栈, 仅用来保存和恢复栈, 不作运行之用
};

struct Scheduler {                    // 协程调度器结构体
    Coroutine **cos;                  // 管理协程的指针数组
    int capacity;                     // 上述数组的长度, 初始为16
    ucontext_t main;                  // 主上下文, 调度器所在函数的上下文
    int running;                      // 当前运行中的协程id, 即为上述数组的索引
    char stack[DEFAULT_STACK_SZIE];   // 栈, 供协程运行时使用, 默认128KB
};

/* 创建一个协程调度器
 */
Scheduler *coroutine_open() {
    Scheduler *scher = calloc(1, sizeof(Scheduler));
    scher->capacity = DEFAULT_COROUTINE_NUM;
    scher->cos = calloc(scher->capacity, sizeof(Coroutine *));
    return scher;
}

/* 删除协程
 */
static void _coroutine_delete(Coroutine *co) {
    if (co == NULL)
        return;
    if (co->stack != NULL)
        free(co->stack);
    free(co);
}

/* 关闭协程调度器, 包括删除所有管理的协程
 * note: 如果关闭时, 有协程处于运行状态,那么会发生段错误,
 *       因为调度器的stack无法释放, 所以不要在协程中调用此函数
 */
void  coroutine_close(Scheduler *s) {
    assert(s != NULL);
    int i;
    Coroutine *co;
    for (i = 0; i < s->capacity; ++i) {
        co = s->cos[i];
        if (co != NULL) {
            _coroutine_delete(co);
        }
    }
    if (s->cos != NULL)
        free(s->cos);
    free(s);
}

/* 创建一个协程
 */
static Coroutine *_coroutine_new(Scheduler *s, CoFunc func, void *arg) {
    Coroutine *co = calloc(1, sizeof(Coroutine));
    co->func = func;
    co->arg = arg;
    co->scher = s;
    co->state = CO_READY;
    
    co->size = 0;
    co->stack = NULL;
    return co; 
}

/* 创建一个协程, 并返回其在调度器中的id
 */
int coroutine_create(Scheduler *s, CoFunc func, void *arg) {
    assert(s != NULL);
    assert(func != NULL);
    int i, cap;
    Coroutine *co = _coroutine_new(s, func, arg);
    cap = s->capacity;
    for (i = 0; i < cap; ++i) {
        if (s->cos[i] == NULL) {
            s->cos[i] = co;
            return i;
        }
    }
    // 如果调度器的管理协程的指针数组容量不够, 扩容为原来的2倍
    s->cos = realloc(s->cos, cap * 2 * sizeof(Coroutine *));
    s->cos[cap] = co;
    return cap;
}

/* 作为makecontext的函数入口,
 * 这里就是对协程的函数入口的封装
 */
static void _coroutine_entry(uintptr_t *arg) {
    Scheduler *s = (Scheduler *)arg;
    assert(s != NULL);
    int cid = s->running;
    assert(cid >= 0 && cid < s->capacity);
    Coroutine *co = s->cos[cid];
    // 调用协程的函数
    co->func(co->arg);
    // 协程函数运行完毕后, 删除协程
    free(co);
    s->cos[cid] = NULL;
    s->running = -1;
}

/* 恢复协程的运行状态
 */
void coroutine_resume(Scheduler *s, int cid) {
    assert(s != NULL);
    assert(cid >= 0 && cid < s->capacity);
    Coroutine *co = s->cos[cid];
    if (co == NULL)
        return;
    
    switch (co->state) {
        case CO_READY: // 协程刚创建完毕, 使用调度器的stack作为它的运行栈
            getcontext(&co->ctx);
            co->ctx.uc_stack.ss_sp = s->stack;
            co->ctx.uc_stack.ss_size = DEFAULT_STACK_SZIE;
            co->ctx.uc_link = &s->main; // 指定后续的上下文, 那么当协程函数运行完毕后, 控制权会返回这里
            s->running = cid;
            co->state = CO_RUNNING;
            makecontext(&co->ctx, (void (*)(void)) _coroutine_entry, 1, (uintptr_t)s);
            swapcontext(&s->main, &co->ctx); // 切换到协程, 并将当前上下文保存到调度器的main中
            break;
        case CO_SUSPEND: // 协程处于挂起状态, 那么只需要恢复其运行栈, 直接切换即可
            // 注意C程序的栈是从高地址向低地址扩展, 所以这里是拷贝到栈的尾部而不是头部
            memcpy(s->stack + DEFAULT_STACK_SZIE - co->size, co->stack, co->size);
            s->running = cid;
            co->state = CO_RUNNING;
            swapcontext(&s->main, &co->ctx);
            break;
        default:
            ;
    }
}

/* 保存协程的栈
 */
static void _save_stack(Coroutine *co, char *top) {
    // 为了确认协程栈的结束地址, 这里创建了一个字符变量, 其地址即为(结束地址-1)
    char dummy = 0;
    // 同样注意栈是从高地址向低地址扩展
    assert(top - &dummy <= DEFAULT_STACK_SZIE);
    if (co->size < top - &dummy) {
        // 如果协程用作保存的stack容量不足, 那么扩容
        co->stack = realloc(co->stack, top - &dummy);
    }
    co->size = top - &dummy;
    memcpy(co->stack, &dummy, co->size);
}

/* 将控制权主动让出给调度器
 */
void coroutine_yield(Scheduler *s) {
    assert(s != NULL);
    int cid = s->running;
    assert(cid >= 0 && cid < s->capacity);
    Coroutine *co = s->cos[cid];
    assert(co != NULL);
    _save_stack(co, s->stack + DEFAULT_STACK_SZIE);
    co->state = CO_SUSPEND;
    s->running = -1;
    swapcontext(&co->ctx, &s->main);
}

/* 获取当前处于运行状态的协程id
 */
int coroutine_running(Scheduler *s) {
    assert(s != NULL);
    return s->running;
}

/* 获取指定id的协程的运行状态
 */
int coroutine_status(Scheduler *s, int cid) {
    assert(s != NULL);
    assert(cid >= 0 && cid < s->capacity);
    Coroutine *co = s->cos[cid];
    return (co != NULL)? co->state: CO_DONE;
}

/* 判断调度器中管理的协程是否全部运行完毕
 */
int coroutine_finished(Scheduler *s) {
    assert(s != NULL);
    int i;
    Coroutine *co;
    for (i = 0; i < s->capacity; ++i) {
        co = s->cos[i];
        if (co != NULL && co->state != CO_DONE)
            return 0;
    }
    return 1;
}
