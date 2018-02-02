#ifndef _COROUTINE
#define _COROUTINE

struct Coroutine;
struct Scheduler;
enum CoState;

typedef void (*CoFunc)(void *arg);

typedef struct Coroutine Coroutine;
typedef struct Scheduler Scheduler;
typedef enum CoState CoState;

Scheduler *coroutine_open();
void  coroutine_close(Scheduler *s);
int coroutine_create(Scheduler *s, CoFunc func, void *arg);
void coroutine_resume(Scheduler *s, int cid);
void coroutine_yield(Scheduler *s);
int coroutine_running(Scheduler *s);
int coroutine_status(Scheduler *s, int cid);
int coroutine_finished(Scheduler *s);

#endif
