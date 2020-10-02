#include "internal/fixnum.h"
#include "ruby/util.h"

// included by ractor.c

#define RS_DEBUG 0

struct tvar_slot {
    uint64_t version;
    VALUE value;
    VALUE index;
    rb_nativethread_lock_t lock_;
#if RS_DEBUG
    int lock_location;
#endif
};

struct ractor_space {
    uint64_t version;

    uint64_t slot_index;
    rb_nativethread_lock_t slot_index_lock;

    // protected by vm_lock
    VALUE tvar_map;
};

struct rstx_slot {
    VALUE value;
    struct tvar_slot *slot;
    VALUE tvar; // mark slot
};

struct ractor_space_tx {
    uint32_t nesting;
    uint32_t version;
    uint32_t copy_cnt;
    uint32_t copy_capa;
    struct rstx_slot *copies;
    bool stop_adding;
};

static struct ractor_space ractor_space;
static VALUE rb_eRactorTxRetry;
static VALUE rb_eRactorTxError;
static VALUE rb_exc_tx_retry;
static VALUE rb_cRactorTVar;

static VALUE rb_cRactorLock;
static VALUE rb_cRactorLVar;

static VALUE
ractor_space_next_index(struct ractor_space *rs)
{
    VALUE index;
    rb_native_mutex_lock(&rs->slot_index_lock);
    {
        rs->slot_index++;
        index = INT2FIX(rs->slot_index);
    }
    rb_native_mutex_unlock(&rs->slot_index_lock);

    return index;
}

static struct ractor_space *
rb_ractor_space(rb_execution_context_t *ec)
{
    return &ractor_space;
}

static uint64_t
ractor_space_version(struct ractor_space *rs)
{
    return rs->version;
}

static uint64_t
ractor_space_new_version(struct ractor_space *rs)
{
    return rs->version + 1;
}

static void
rs_slot_lock_(struct tvar_slot *slot, int line)
{
    rb_native_mutex_lock(&slot->lock_);

#if RS_DEBUG
    slot->lock_location = line;
#endif
}

static void
rs_slot_unlock_(struct tvar_slot *slot, int line)
{
#if RS_DEBUG
    slot->lock_location = 0;
#endif

    rb_native_mutex_unlock(&slot->lock_);
}

#define rs_slot_lock(slot) rs_slot_lock_(slot, __LINE__)
#define rs_slot_unlock(slot) rs_slot_unlock_(slot, __LINE__)

// tx: transaction

static struct ractor_space_tx *
ractor_space_tx(rb_ractor_t *cr)
{
    if (UNLIKELY(cr->tx == NULL)) {
        cr->tx = ZALLOC(struct ractor_space_tx);
        // cr->tx->nesting = 0;
        // cr->tx->version = 0;
        // cr->tx->stop_adding = false;
        // cr->tx->copy_cnt = 0;
        cr->tx->copy_capa = 0x10; // default
        cr->tx->copies = ALLOC_N(struct rstx_slot, cr->tx->copy_capa);
    }
    return cr->tx;
}

struct rstx_slot *
ractor_space_tx_lookup(struct ractor_space_tx *tx, VALUE tvar)
{
    struct rstx_slot *copies = tx->copies;
    uint32_t cnt = tx->copy_cnt;

    for (uint32_t i = 0; i< cnt; i++) {
        if (copies[i].tvar == tvar) {
            return &copies[i];
        }
    }

    return NULL;
}

static void
ractor_space_tx_add(struct ractor_space_tx *tx, VALUE val, struct tvar_slot *slot, VALUE tvar)
{
    if (UNLIKELY(tx->copy_capa == tx->copy_cnt)) {
        uint32_t new_capa =  tx->copy_capa * 2;
        SIZED_REALLOC_N(tx->copies, struct rstx_slot, new_capa, tx->copy_capa);
        tx->copy_capa = new_capa;
    }
    if (UNLIKELY(tx->stop_adding)) {
        rb_raise(rb_eRactorTxError, "can not handle more transactional variable: %"PRIxVALUE, rb_inspect(tvar));
    }
    struct rstx_slot *ent = &tx->copies[tx->copy_cnt++];

    ent->value = val;
    ent->slot = slot;
    ent->tvar = tvar;
}

static VALUE
ractor_space_tx_get(struct ractor_space_tx *tx, struct tvar_slot *slot, VALUE tvar)
{
    struct rstx_slot *ent = ractor_space_tx_lookup(tx, tvar);

    if (ent == NULL) {
        VALUE val = slot->value;
        ractor_space_tx_add(tx, val, slot, tvar);
        return val;
    }
    else {
        return ent->value;
    }
}

static void
ractor_space_tx_set(struct ractor_space_tx *tx, VALUE val, struct tvar_slot *slot, VALUE tvar)
{
    struct rstx_slot *ent = ractor_space_tx_lookup(tx, tvar);

    if (ent == NULL) {
        ractor_space_tx_add(tx, val, slot, tvar);
    }
    else {
        ent->value = val;
    }
}

static void
ractor_space_tx_check(struct ractor_space_tx *tx)
{
    if (UNLIKELY(tx->nesting == 0)) {
        rb_raise(rb_eRactorTxError, "can not set without transaction");
    }
}

static VALUE
ractor_space_tx_begin(rb_execution_context_t *ec, VALUE self)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);

    VM_ASSERT(tx->stop_adding == false);

    tx->nesting++;
    if (tx->nesting == 1) {
        tx->version = ractor_space_version(rs);
        return Qtrue;
    }
    else {
        tx->nesting++;
        return Qfalse;
    }
}

static VALUE
ractor_space_tx_reset(rb_execution_context_t *ec, VALUE self)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    VM_ASSERT(tx->nesting == 1);

    tx->version = ractor_space_version(rs);
    tx->copy_cnt = 0;
    return Qnil;
}

static VALUE
ractor_space_tx_end(rb_execution_context_t *ec, VALUE self)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    VM_ASSERT(tx->nesting > 0);
    VM_ASSERT(tx->stop_adding == false);
    tx->nesting--;
    tx->copy_cnt = 0;
    return Qnil;
}

static int
rstx_slot_cmp(const void *p1, const void *p2, void *dmy)
{
    const struct rstx_slot *s1 = (struct rstx_slot *)p1;
    const struct rstx_slot *s2 = (struct rstx_slot *)p2;
    VALUE i1 = s1->slot->index;
    VALUE i2 = s2->slot->index;

    return i1 < i2 ? 1 : i1 > i2 ? -1 : 0;
}

static void
ractor_space_tx_sort(struct ractor_space_tx *tx)
{
    switch (tx->copy_cnt) {
      case 0:
      case 1:
        break;
      default:
        ruby_qsort(tx->copies, tx->copy_cnt, sizeof(struct rstx_slot), rstx_slot_cmp, NULL);
        break;
    }
}

static VALUE
ractor_space_tx_commit(rb_execution_context_t *ec, VALUE self, VALUE ret)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    uint32_t i, j;

    ractor_space_tx_sort(tx);

    for (i=0; i<tx->copy_cnt; i++) {
        struct rstx_slot *copy = &tx->copies[i];
        struct tvar_slot *slot = copy->slot;

        rs_slot_lock(slot);
        if (slot->version > tx->version) {
            // retry
            for (j=0; j<=i; j++) {
                struct rstx_slot *copy = &tx->copies[j];
                struct tvar_slot *slot = copy->slot;
                rs_slot_unlock(slot);
            }

            rb_exc_raise(rb_exc_tx_retry);
        }
    }

    uint64_t new_version = ractor_space_new_version(rs);

    for (i=0; i<tx->copy_cnt; i++) {
        struct rstx_slot *copy = &tx->copies[i];
        struct tvar_slot *slot = copy->slot;

        if (slot->value != copy->value) {
            slot->version = new_version;
            slot->value = copy->value;
        }
    }

    rs->version = new_version;

    for (i=0; i<tx->copy_cnt; i++) {
        struct rstx_slot *copy = &tx->copies[i];
        struct tvar_slot *slot = copy->slot;
        rs_slot_unlock(slot);
    }

    return ret;
}

// ruby-level API

static VALUE
ractor_space_get(rb_execution_context_t *ec, VALUE key)
{
    rb_bug("TODO");
}

static VALUE
ractor_space_set(rb_execution_context_t *ec, VALUE key, VALUE val)
{
    rb_bug("TODO");
}

static void
ractor_tvar_mark(void *ptr)
{
    struct tvar_slot *slot = (struct tvar_slot *)ptr;
    rb_gc_mark(slot->value);
}

static const rb_data_type_t tvar_data_type = {
    "Ractor::TVar",
    {ractor_tvar_mark, RUBY_TYPED_DEFAULT_FREE, NULL,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE
ractor_space_tvar_new(rb_execution_context_t *ec, VALUE self, VALUE init)
{
    struct ractor_space *rs = rb_ractor_space(ec);
    struct tvar_slot *slot;

    VALUE obj = TypedData_Make_Struct(rb_cRactorTVar, struct tvar_slot, &tvar_data_type, slot);
    slot->version = 0;
    slot->value = init;
    slot->index = ractor_space_next_index(rs);
    rb_native_mutex_initialize(&slot->lock_);

    rb_obj_freeze(obj);
    FL_SET_RAW(obj, RUBY_FL_SHAREABLE);

    return obj;
}

static VALUE
ractor_space_tvar_value(rb_execution_context_t *ec, VALUE self)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    ractor_space_tx_check(tx);
    struct tvar_slot *slot = DATA_PTR(self);
    return ractor_space_tx_get(tx, slot, self);
}

static VALUE
ractor_space_tvar_value_set(rb_execution_context_t *ec, VALUE self, VALUE val)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    ractor_space_tx_check(tx);
    struct tvar_slot *slot = DATA_PTR(self);
    ractor_space_tx_set(tx, val, slot, self);
    return val;
}

static VALUE
ractor_space_calc_inc(VALUE v, VALUE inc)
{
    if (LIKELY(FIXNUM_P(v) && FIXNUM_P(inc))) {
        VALUE r = rb_fix_plus_fix(v, inc);
        return r;
    }
    else {
        rb_bug("unsupported");
        // return rb_funcall(v, rb_intern("+"), inc);
    }
}

static VALUE
ractor_space_tvar_value_increment(rb_execution_context_t *ec, VALUE self, VALUE inc)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    VALUE v;
    struct tvar_slot *slot = DATA_PTR(self);

    if (tx->nesting == 0) {
        rs_slot_lock(slot);
        {
            uint64_t new_version = ractor_space_new_version(rs);
            v = slot->value;

            // TODO: should be fixnum
            v = ractor_space_calc_inc(v, inc);

            slot->value = v;
            slot->version = new_version;

            // TODO: memory reorder?
            rs->version = new_version;
        }
        rs_slot_unlock(slot);
    }
    else {
        v = ractor_space_tx_get(tx, slot, self);
        v = ractor_space_calc_inc(v, inc);
        ractor_space_tx_set(tx, v, slot, self);
    }

    return v;
}

struct ractor_lock {
    rb_nativethread_cond_t cond;
    rb_nativethread_lock_t lock;
    rb_thread_t *owner;

    int ok, ng;
};

#if 0
static void
ractor_lock_free(void *ptr)
{
    struct ractor_lock *lock = (struct ractor_lock *)ptr;
    fprintf(stderr, "ok:%d ng:%d\n", lock->ok, lock->ng);
}
#else
#define ractor_lock_free RUBY_TYPED_DEFAULT_FREE
#endif

static const rb_data_type_t lock_data_type = {
    "Ractor::Lock",
    {NULL, ractor_lock_free, NULL,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static void *
slot_lock(void *ptr)
{
    struct tvar_slot *slot = (struct tvar_slot *)ptr;
    rs_slot_lock(slot);
    return NULL;
}

struct tvar_slot *
tvar_slot_ptr(VALUE v)
{
    if (rb_typeddata_is_kind_of(v, &tvar_data_type)) {
        return DATA_PTR(v);
    }
    else {
        rb_raise(rb_eArgError, "TVar is needed");
    }
}

static VALUE
ractor_space_lock_begin(rb_execution_context_t *ec, VALUE self, VALUE tvars)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    int i;

    if (tx->nesting > 0) {
        rb_raise(rb_eRactorTxError, "can not nest lock");
    }

    tx->nesting = 1;

    int len = RARRAY_LENINT(tvars);
    for (i=0; i<len; i++) {
        VALUE tvar = RARRAY_AREF(tvars, i);
        struct tvar_slot *slot = tvar_slot_ptr(tvar);

        rb_thread_call_without_gvl(slot_lock, slot, NULL, NULL);
        ractor_space_tx_add(tx, slot->value, slot, tvar);
    }

    tx->stop_adding = true;

    return Qtrue;
}

static VALUE
ractor_space_lock_commit(rb_execution_context_t *ec, VALUE self)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);

    ractor_space_tx_sort(tx);

    uint64_t new_version = ractor_space_new_version(rs);

    for (uint32_t i=0; i<tx->copy_cnt; i++) {
        struct rstx_slot *copy = &tx->copies[i];
        struct tvar_slot *slot = copy->slot;

        if (slot->value != copy->value) {
            slot->version = new_version;
            slot->value = copy->value;
        }
    }

    rs->version = new_version;

    return Qnil;
}

static VALUE
ractor_space_lock_end(rb_execution_context_t *ec, VALUE self)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    VM_ASSERT(tx->nesting > 0);

    // release lock
    for (uint32_t i=0; i<tx->copy_cnt; i++) {
        rs_slot_unlock(tx->copies[i].slot);
    }

    tx->nesting = 0;
    tx->copy_cnt = 0;
    tx->stop_adding = false;
    return Qnil;
}

static VALUE
ractor_lock_new(rb_execution_context_t *ec, VALUE self)
{
    struct ractor_lock *lock;
    VALUE obj = TypedData_Make_Struct(rb_cRactorLock, struct ractor_lock, &lock_data_type, lock);
    rb_native_mutex_initialize(&lock->lock);
    rb_native_cond_initialize(&lock->cond);
    rb_obj_freeze(obj);
    FL_SET_RAW(obj, RUBY_FL_SHAREABLE);

    lock->ok = lock->ng = 0;
    return obj;
}

struct lock_lock_data {
    struct ractor_lock *lock;
    rb_execution_context_t *ec;
};

static void *
lock_lock(void *ptr)
{
    struct lock_lock_data *data = (struct lock_lock_data *)ptr;
    struct ractor_lock *lock = data->lock;

    rb_native_mutex_lock(&lock->lock);
    {
        while (lock->owner != NULL) {
            rb_native_cond_wait(&lock->cond, &lock->lock);

            if (lock->owner != NULL) {
                lock->ok++;
            }
            else {
                lock->ng++;
            }
        }
        lock->owner = rb_ec_thread_ptr(data->ec);
    }
    rb_native_mutex_unlock(&lock->lock);

    return NULL;
}

static VALUE
ractor_lock_lock(rb_execution_context_t *ec, VALUE self)
{
    struct ractor_lock *lock = DATA_PTR(self);
    struct lock_lock_data data = {
        lock, ec
    };
    rb_thread_call_without_gvl(lock_lock, &data, NULL, NULL);
    return Qfalse;
}

static VALUE
ractor_lock_unlock(rb_execution_context_t *ec, VALUE self)
{
    struct ractor_lock *lock = DATA_PTR(self);

    rb_native_mutex_lock(&lock->lock);
    lock->owner = NULL;
    rb_native_cond_signal(&lock->cond);
    rb_native_mutex_unlock(&lock->lock);

    return Qfalse;
}

static VALUE
ractor_lock_own_p(rb_execution_context_t *ec, VALUE self)
{
    struct ractor_lock *lock = DATA_PTR(self);
    return lock->owner == rb_ec_thread_ptr(ec) ? Qtrue : Qfalse;
}

struct ractor_lvar {
    VALUE lock;
    VALUE value;
};

static void
lvar_mark(void *ptr)
{
    struct ractor_lvar *lvar = (struct ractor_lvar *)ptr;
    rb_gc_mark(lvar->lock);
    rb_gc_mark(lvar->value);
}

static const rb_data_type_t lvar_data_type = {
    "Ractor::LVar",
    {lvar_mark, RUBY_TYPED_DEFAULT_FREE, NULL,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE
ractor_lvar_new(rb_execution_context_t *ec, VALUE self, VALUE init, VALUE lock)
{
    struct ractor_lvar *lvar;
    VALUE obj = TypedData_Make_Struct(rb_cRactorLVar, struct ractor_lvar, &lvar_data_type, lvar);
    lvar->lock = lock;
    lvar->value = init;
    rb_obj_freeze(obj);
    FL_SET_RAW(obj, RUBY_FL_SHAREABLE);
    return obj;
}

static VALUE
ractor_lvar_value(rb_execution_context_t *ec, VALUE self)
{
    struct ractor_lvar *lvar = DATA_PTR(self);
    if (UNLIKELY(!ractor_lock_own_p(ec, lvar->lock))) {
        rb_raise(rb_eRactorError, "corresponding lock is not acquired");
    }
    return lvar->value;
}

static VALUE
ractor_lvar_value_set(rb_execution_context_t *ec, VALUE self, VALUE val)
{
    struct ractor_lvar *lvar = DATA_PTR(self);
    if (UNLIKELY(!ractor_lock_own_p(ec, lvar->lock))) {
        rb_raise(rb_eRactorError, "corresponding lock is not acquired");
    }
    if (UNLIKELY(!rb_ractor_shareable_p(val))) {
        rb_raise(rb_eRactorError, "only shareable object are allowed");
    }
    return lvar->value = val;
}

static void
Init_ractor_space(void)
{
    struct ractor_space *rs = rb_ractor_space(GET_EC());
    rs->slot_index = 0;
    rs->version = 0;
    rb_native_mutex_initialize(&rs->slot_index_lock);

    rb_eRactorTxError = rb_define_class_under(rb_cRactor, "TransactionError", rb_eRuntimeError);
    rb_eRactorTxRetry = rb_define_class_under(rb_cRactor, "RetryTransaction", rb_eException);

    rb_cRactorTVar = rb_define_class_under(rb_cRactor, "TVar", rb_cObject);
    rb_cRactorLock = rb_define_class_under(rb_cRactor, "Lock", rb_cObject);
    rb_cRactorLVar = rb_define_class_under(rb_cRactor, "LVar", rb_cObject);

    rb_exc_tx_retry = rb_exc_new_cstr(rb_eRactorTxRetry, "Ractor::RetryTransaction");
    rb_obj_freeze(rb_exc_tx_retry);
    rb_gc_register_mark_object(rb_exc_tx_retry);
}
