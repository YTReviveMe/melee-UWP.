#pragma once

#include <windows.h>

typedef struct {
    INIT_ONCE once;
    CRITICAL_SECTION section;
} pthread_mutex_t;
typedef int pthread_mutexattr_t;
typedef INIT_ONCE pthread_once_t;

#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT
#define PTHREAD_MUTEX_INITIALIZER { INIT_ONCE_STATIC_INIT }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_RECURSIVE 1

static BOOL CALLBACK xbox_mutex_init_once(PINIT_ONCE once, PVOID parameter, PVOID* context) {
    (void)once;
    (void)context;
    InitializeCriticalSection(&((pthread_mutex_t*)parameter)->section);
    return TRUE;
}

static BOOL CALLBACK xbox_call_once(PINIT_ONCE once, PVOID parameter, PVOID* context) {
    (void)once;
    (void)context;
    ((void (*)(void))parameter)();
    return TRUE;
}

static int pthread_once(pthread_once_t* once, void (*fn)(void)) {
    return InitOnceExecuteOnce(once, xbox_call_once, (PVOID)fn, NULL) ? 0 : -1;
}
static int pthread_mutexattr_init(pthread_mutexattr_t* attr) { *attr = 0; return 0; }
static int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) { (void)attr; (void)type; return 0; }
static int pthread_mutexattr_destroy(pthread_mutexattr_t* attr) { (void)attr; return 0; }
static int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    (void)attr;
    return InitOnceExecuteOnce(&mutex->once, xbox_mutex_init_once, mutex, NULL) ? 0 : -1;
}
static int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (!InitOnceExecuteOnce(&mutex->once, xbox_mutex_init_once, mutex, NULL)) return -1;
    EnterCriticalSection(&mutex->section);
    return 0;
}
static int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    LeaveCriticalSection(&mutex->section);
    return 0;
}
