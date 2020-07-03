#include "vm_core.h"
#include "vm_sync.h"
#include "ractor.h"
#include "vm_debug.h"
#include "gc.h"

static bool
vm_locked(rb_vm_t *vm)
{
    return vm->ractor.sync.lock_owner == GET_RACTOR();
}

#if VM_CHECK_MODE > 0
void
ASSERT_vm_locking(void)
{
    rb_vm_t *vm = GET_VM();
    VM_ASSERT(vm_locked(vm));
}
#endif

#if VM_CHECK_MODE > 0
void
ASSERT_vm_unlocking(void)
{
    rb_vm_t *vm = GET_VM();
    VM_ASSERT(!vm_locked(vm));
}
#endif

static bool vm_barrier_finish_p(rb_vm_t *vm);

static void
vm_lock_enter(rb_vm_t *vm, bool locked, unsigned int *lev APPEND_LOCATION_ARGS)
{
    if (locked) {
        ASSERT_vm_locking();
    }
    else {
#if RACTOR_CHECK_MODE
        // locking ractor and acquire VM lock will cause deadlock
        rb_ractor_t *r = GET_RACTOR();
        VM_ASSERT(r->locked_by != r->self);
#endif

        // lock
        rb_native_mutex_lock(&vm->ractor.sync.lock);

        // barrier
        while (vm->ractor.sync.barrier_waiting) {
            unsigned int barrier_cnt = vm->ractor.sync.barrier_cnt;
            rb_ractor_t *cr = GET_RACTOR();
            rb_thread_t *th = GET_THREAD();
            bool running;

            RB_GC_SAVE_MACHINE_CONTEXT(th);

            if (cr->threads.cnt != cr->threads.blocking_cnt) {
                // running ractor
                rb_vm_ractor_blocking_cnt_inc(vm, __FILE__, __LINE__);
                running = true;
            }
            else {
                running = false;
            }

            if (vm_barrier_finish_p(vm)) {
                RUBY_DEBUG_LOG("wakeup barrier owner", 0);
                rb_native_cond_signal(&vm->ractor.sync.barrier_cond);
            }
            else {
                RUBY_DEBUG_LOG("wait for barrier finish", 0);
            }

            // wait for restart
            while (barrier_cnt == vm->ractor.sync.barrier_cnt) {
                rb_native_cond_wait(&cr->barrier_wait_cond, &vm->ractor.sync.lock);
            }

            RUBY_DEBUG_LOG("barrier is released. Acquire vm_lock", 0);

            if (running) {
                rb_vm_ractor_blocking_cnt_dec(vm, __FILE__, __LINE__);
            }
        }

        VM_ASSERT(vm->ractor.sync.lock_rec == 0);
        VM_ASSERT(vm->ractor.sync.lock_owner == NULL);

        vm->ractor.sync.lock_owner = GET_RACTOR();
    }

    vm->ractor.sync.lock_rec++;
    *lev = vm->ractor.sync.lock_rec;

    RUBY_DEBUG_LOG2(file, line, "rec:%u owner:%d", vm->ractor.sync.lock_rec, rb_ractor_id(vm->ractor.sync.lock_owner));
}

static void
vm_lock_leave(rb_vm_t *vm, unsigned int *lev APPEND_LOCATION_ARGS)
{
    RUBY_DEBUG_LOG2(file, line, "rec:%u owner:%d", vm->ractor.sync.lock_rec, rb_ractor_id(vm->ractor.sync.lock_owner));

    ASSERT_vm_locking();
    VM_ASSERT(vm->ractor.sync.lock_rec > 0);
    VM_ASSERT(vm->ractor.sync.lock_rec == *lev);

    vm->ractor.sync.lock_rec--;

    if (vm->ractor.sync.lock_rec == 0) {
        vm->ractor.sync.lock_owner = NULL;
        rb_native_mutex_unlock(&vm->ractor.sync.lock);
    }
}

void
rb_vm_lock_enter_body(unsigned int *lev APPEND_LOCATION_ARGS)
{
    rb_vm_t *vm = GET_VM();
    vm_lock_enter(vm, vm_locked(vm), lev APPEND_LOCATION_PARAMS);
}

void
rb_vm_lock_leave_body(unsigned int *lev APPEND_LOCATION_ARGS)
{
    vm_lock_leave(GET_VM(), lev APPEND_LOCATION_PARAMS);
}

void
rb_vm_lock_body(LOCATION_ARGS)
{
    rb_vm_t *vm = GET_VM();
    ASSERT_vm_unlocking();
    vm_lock_enter(vm, false, &vm->ractor.sync.lock_rec APPEND_LOCATION_PARAMS);
}

void
rb_vm_unlock_body(LOCATION_ARGS)
{
    rb_vm_t *vm = GET_VM();
    ASSERT_vm_locking();
    VM_ASSERT(vm->ractor.sync.lock_rec == 1);
    vm_lock_leave(vm, &vm->ractor.sync.lock_rec APPEND_LOCATION_PARAMS);
}

static bool
vm_barrier_finish_p(rb_vm_t *vm)
{
    RUBY_DEBUG_LOG("cnt:%u living:%u blocking:%u",
                   vm->ractor.sync.barrier_cnt,
                   vm->ractor.cnt,
                   vm->ractor.blocking_cnt);
    VM_ASSERT(vm->ractor.blocking_cnt <= vm->ractor.cnt);
    return vm->ractor.blocking_cnt == vm->ractor.cnt;
}

void
vm_cond_wait(rb_vm_t *vm, rb_nativethread_cond_t *cond, unsigned long msec)
{
    ASSERT_vm_locking();
    unsigned int lock_rec = vm->ractor.sync.lock_rec;
    rb_ractor_t *cr = vm->ractor.sync.lock_owner;

    vm->ractor.sync.lock_rec = 0;
    vm->ractor.sync.lock_owner = NULL;
    if (msec > 0) {
        rb_native_cond_timedwait(cond, &vm->ractor.sync.lock, msec);
    }
    else {
        rb_native_cond_wait(cond, &vm->ractor.sync.lock);
    }
    vm->ractor.sync.lock_rec = lock_rec;
    vm->ractor.sync.lock_owner = cr;
}

void
rb_vm_cond_wait(rb_vm_t *vm, rb_nativethread_cond_t *cond)
{
    vm_cond_wait(vm, cond, 0);
}

void
rb_vm_cond_timedwait(rb_vm_t *vm, rb_nativethread_cond_t *cond, unsigned long msec)
{
    vm_cond_wait(vm, cond, msec);
}

void
rb_vm_barrier(void)
{
    if (!rb_multi_ractor_p()) {
        // no other ractors
        return;
    }
    else {
        rb_vm_t *vm = GET_VM();
        ASSERT_vm_locking();
        VM_ASSERT(vm->ractor.sync.barrier_waiting == false);
        rb_ractor_t *r;
        rb_ractor_t *cr = vm->ractor.sync.lock_owner;
        VM_ASSERT(cr == GET_RACTOR());

        vm->ractor.sync.barrier_waiting = true;

        RUBY_DEBUG_LOG("barrier start. cnt:%u living:%u blocking:%u",
                       vm->ractor.sync.barrier_cnt,
                       vm->ractor.cnt,
                       vm->ractor.blocking_cnt);

        rb_vm_ractor_blocking_cnt_inc(vm, __FILE__, __LINE__);

        // send signal
        list_for_each(&vm->ractor.set, r, vmlr_node) {
            if (r != cr) {
                rb_ractor_vm_barrier_interrupt_running_thread(r);
            }
        }

        // wait
        while (!vm_barrier_finish_p(vm)) {
            rb_vm_cond_wait(vm, &vm->ractor.sync.barrier_cond);
        }

        RUBY_DEBUG_LOG("cnt:%u barrier success", vm->ractor.sync.barrier_cnt);

        rb_vm_ractor_blocking_cnt_dec(vm, __FILE__, __LINE__);

        vm->ractor.sync.barrier_waiting = false;
        vm->ractor.sync.barrier_cnt++;

        list_for_each(&vm->ractor.set, r, vmlr_node) {
            rb_native_cond_signal(&r->barrier_wait_cond);
        }
    }
}
