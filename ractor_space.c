#include "internal/fixnum.h"

// included by ractor.c

#define RS_DEBUG 0

struct rs_slot {
    uint64_t version;
    VALUE value;
    VALUE name;
    rb_nativethread_lock_t lock_;
#if RS_DEBUG
    int lock_location;
#endif
};

#define RS_PAGE_SLOTS  512
#define RS_MAX_PAGES  1024
#define RS_MAX_SLOTS  (RS_MAX_PAGES * RS_PAGE_SLOTS)

struct ractor_space {
    uint32_t slot_cnt;
    struct rs_slot *pages[RS_MAX_PAGES];
    uint64_t version;
    rb_nativethread_lock_t slots_lock;
};

struct rstx_slot {
    VALUE key;
    VALUE value;
    struct rs_slot *slot;
    uint32_t index;
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
static VALUE rb_eRactorSpaceError;
static VALUE rb_eRactorSpaceRetry;
static VALUE rb_eRactorSpaceTransactionError;
static VALUE rb_cRactorSpaceTVar;

static VALUE rb_cRactorLock;
static VALUE rb_cRactorLVar;

static uint32_t
ractor_space_next_index(struct ractor_space *rs)
{
    uint32_t index;
    rb_native_mutex_lock(&rs->slots_lock);
    {
        index = rs->slot_cnt++;
    }
    rb_native_mutex_unlock(&rs->slots_lock);

    return index;
}

static uint32_t
ractor_space_name2index(struct ractor_space *rs, VALUE key)
{
    rb_bug("TODO: fix later");
}

static struct rs_slot *
ractor_space_new_page(void)
{
    struct rs_slot *slots = ALLOC_N(struct rs_slot, RS_PAGE_SLOTS);
    for (int i=0; i<RS_PAGE_SLOTS; i++) {
        struct rs_slot *slot = &slots[i];
        slot->version = 0;
        slot->value = Qnil;
        slot->name = Qnil;
        rb_native_mutex_initialize(&slot->lock_);
    }
    return slots;
}

static struct rs_slot *
ractor_space_index_ref(struct ractor_space *rs, uint32_t index)
{
    return &rs->pages[index/RS_PAGE_SLOTS][index%RS_PAGE_SLOTS];
}

static void
ractor_space_prepare_slot(struct ractor_space *rs, uint32_t index)
{
    if (UNLIKELY(index >= RS_MAX_SLOTS)) {
        rb_raise(rb_eRactorSpaceError, "can not make TVars more");
    }

    if (rs->pages[index / RS_PAGE_SLOTS] == NULL) {
        rb_native_mutex_lock(&rs->slots_lock);
        if (rs->pages[index / RS_PAGE_SLOTS] == NULL) {
            rs->pages[index / RS_PAGE_SLOTS] = ractor_space_new_page();
        }
        rb_native_mutex_unlock(&rs->slots_lock);
    }
}

static uint32_t
ractor_space_index(struct ractor_space *rs, VALUE key)
{
    uint32_t index;

    if (FIXNUM_P(key)) {
        index =  FIX2UINT(key);
    }
    else {
        index = ractor_space_name2index(rs, key);
    }
    ractor_space_prepare_slot(rs, index);
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
rs_slot_lock_(struct rs_slot *slot, int line)
{
    rb_native_mutex_lock(&slot->lock_);

#if RS_DEBUG
    slot->lock_location = line;
#endif
}

static void
rs_slot_unlock_(struct rs_slot *slot, int line)
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
ractor_space_tx_lookup(struct ractor_space_tx *tx, VALUE key)
{
    struct rstx_slot *copies = tx->copies;
    uint32_t cnt = tx->copy_cnt;

    for (uint32_t i = 0; i< cnt; i++) {
        if (copies[i].key == key) {
            return &copies[i];
        }
    }

    return NULL;
}

static void
ractor_space_tx_add(struct ractor_space_tx *tx, VALUE key, VALUE val, uint32_t index, struct rs_slot *slot)
{
    if (tx->copy_capa == tx->copy_cnt) {
        rb_bug("TODO");
    }
    if (tx->stop_adding) {
        rb_raise(rb_eRactorSpaceTransactionError, "can not handle more transactional variable: %"PRIxVALUE, rb_inspect(key));
    }
    struct rstx_slot *ent = &tx->copies[tx->copy_cnt++];

    ent->key = key;
    ent->value = val;
    ent->index = index;
    ent->slot = slot;
}

static VALUE
ractor_space_tx_get(struct ractor_space *rs, struct ractor_space_tx *tx, VALUE key)
{
    struct rstx_slot *ent = ractor_space_tx_lookup(tx, key);

    if (ent == NULL) {
        uint32_t index = ractor_space_index(rs, key);
        struct rs_slot *slot = ractor_space_index_ref(rs, index);
        VALUE val = slot->value;
        ractor_space_tx_add(tx, key, val, index, slot);
        return val;
    }
    else {
        return ent->value;
    }
}

static void
ractor_space_tx_set(struct ractor_space *rs, struct ractor_space_tx *tx, VALUE key, VALUE val)
{
    struct rstx_slot *ent = ractor_space_tx_lookup(tx, key);

    if (ent == NULL) {
        uint32_t index = ractor_space_index(rs, key);
        struct rs_slot *slot = ractor_space_index_ref(rs, index);
        ractor_space_tx_add(tx, key, val, index, slot);
    }
    else {
        ent->value = val;
    }
}

static void
ractor_space_tx_check(struct ractor_space_tx *tx)
{
    if (tx->nesting == 0) {
        rb_raise(rb_eRactorSpaceTransactionError, "can not set without transaction");
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

static void
ractor_space_tx_sort(struct ractor_space_tx *tx)
{
    // TODO
}

static VALUE
ractor_space_tx_commit(rb_execution_context_t *ec, VALUE self)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    uint32_t i, j;

    ractor_space_tx_sort(tx);

    for (i=0; i<tx->copy_cnt; i++) {
        struct rstx_slot *copy = &tx->copies[i];
        struct rs_slot *slot = copy->slot;

        rs_slot_lock(slot);
        if (slot->version > tx->version) {
            // retry
            for (j=0; j<=i; j++) {
                struct rstx_slot *copy = &tx->copies[j];
                struct rs_slot *slot = copy->slot;
                rs_slot_unlock(slot);
                
            }
            // fprintf(stderr, "i:%d slot:%d tx:%d v:%d\n", (int)i, (int)slot->version, (int)tx->version, (int)FIX2INT(copy->value));
            rb_raise(rb_eRactorSpaceRetry, "commit is not success."); // TODO
        }
    }

    uint64_t new_version = ractor_space_new_version(rs);

    for (i=0; i<tx->copy_cnt; i++) {
        struct rstx_slot *copy = &tx->copies[i];
        struct rs_slot *slot = copy->slot;

        if (slot->value != copy->value) {
            slot->version = new_version;
            slot->value = copy->value;
        }
    }

    rs->version = new_version;

    for (i=0; i<tx->copy_cnt; i++) {
        struct rstx_slot *copy = &tx->copies[i];
        struct rs_slot *slot = copy->slot;
        rs_slot_unlock(slot);
    }

    return Qnil;
}

// pessimistic lock

static VALUE
tvar2key(VALUE tvar)
{
    if (FIXNUM_P(tvar)) {
        return tvar;
    }
    if (SYMBOL_P(tvar)) {
        return tvar;
    }
    if (rb_obj_is_kind_of(tvar, rb_cRactorSpaceTVar)) {
        return rb_ivar_get(tvar, rb_intern("index"));
    }
    rb_raise(rb_eRactorSpaceError, "not a tvar");
}

static void *
slot_lock(void *ptr)
{
    struct rs_slot *slot = (struct rs_slot *)ptr;
    rs_slot_lock(slot);
    return NULL;
}

static VALUE
ractor_space_lock_begin(rb_execution_context_t *ec, VALUE self, VALUE tvars)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    int i;

    if (tx->nesting > 0) {
        rb_raise(rb_eRactorSpaceTransactionError, "can not nest lock");
    }

    tx->nesting = 1;

    int len = RARRAY_LENINT(tvars);
    for (i=0; i<len; i++) {
        VALUE tvar = RARRAY_AREF(tvars, i);
        VALUE key = tvar2key(tvar);
        uint32_t index = ractor_space_index(rs, key);
        struct rs_slot *slot = ractor_space_index_ref(rs, index);
        rb_thread_call_without_gvl(slot_lock, slot, NULL, NULL);
        ractor_space_tx_add(tx, key, slot->value, index, slot);
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

    // TODO: sort

    uint64_t new_version = ractor_space_new_version(rs);

    for (uint32_t i=0; i<tx->copy_cnt; i++) {
        struct rstx_slot *copy = &tx->copies[i];
        struct rs_slot *slot = copy->slot;

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

// ruby-level API

static VALUE
ractor_space_get(rb_execution_context_t *ec, VALUE key)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);
    ractor_space_tx_check(tx);
    return ractor_space_tx_get(rs, tx, key);
}

static VALUE
ractor_space_set(rb_execution_context_t *ec, VALUE key, VALUE val)
{
    rb_ractor_t *cr = rb_ec_ractor_ptr(ec);
    struct ractor_space *rs = rb_ractor_space(ec);
    struct ractor_space_tx *tx = ractor_space_tx(cr);

    ractor_space_tx_check(tx);

    if (!rb_ractor_shareable_p(val)) {
        rb_raise(rb_eRactorSpaceError, "can not set an unshareable value");
    }

    ractor_space_tx_set(rs, tx, key, val);
    return Qnil;
}

static VALUE
ractor_space_tvar_new(rb_execution_context_t *ec, VALUE self, VALUE init)
{
    VALUE obj = rb_obj_alloc(rb_cRactorSpaceTVar);
    struct ractor_space *rs = rb_ractor_space(ec);
    uint32_t index = ractor_space_next_index(rs);
    VALUE vi = INT2FIX(index);
    ractor_space_prepare_slot(rs, index);
    struct rs_slot *slot = ractor_space_index_ref(rs, index);
    rb_ivar_set(obj, rb_intern("index"), vi);
    slot->value = init;
    rb_obj_freeze(obj);
    FL_SET_RAW(obj, RUBY_FL_SHAREABLE);
    return obj;
}

static VALUE
ractor_space_tvar_value(rb_execution_context_t *ec, VALUE self)
{
    VALUE key = rb_ivar_get(self, rb_intern("index"));
    return ractor_space_get(ec, key);
}

static VALUE
ractor_space_tvar_value_set(rb_execution_context_t *ec, VALUE self, VALUE val)
{
    VALUE key = rb_ivar_get(self, rb_intern("index"));
    return ractor_space_set(ec, key, val);
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
    VALUE key = rb_ivar_get(self, rb_intern("index"));
    VALUE v;

    if (tx->nesting == 0) {
        uint32_t index = FIX2INT(key);
        struct rs_slot *slot = ractor_space_index_ref(rs, index);
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
        // Ractor[key] += inc
        v = ractor_space_tx_get(rs, tx, key);
        v = ractor_space_calc_inc(v, inc);
        ractor_space_tx_set(rs, tx, key, v);
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

static const rb_data_type_t lvar_data_type = {
    "Ractor::LVar",
    {NULL, RUBY_TYPED_DEFAULT_FREE, NULL,},
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
    rs->slot_cnt = 0;
    rs->version = 0;
    rb_native_mutex_initialize(&rs->slots_lock);
    
    VALUE rb_cRactorSpace = rb_define_class_under(rb_cRactor, "Space", rb_cObject);
    rb_eRactorSpaceError = rb_define_class_under(rb_cRactorSpace, "Error", rb_eRuntimeError);
    rb_eRactorSpaceTransactionError = rb_define_class_under(rb_cRactorSpace, "TransactionError", rb_eRuntimeError);
    rb_eRactorSpaceRetry = rb_define_class_under(rb_cRactorSpace, "Retry", rb_eException);

    rb_cRactorSpaceTVar = rb_define_class_under(rb_cRactorSpace, "TVar", rb_cObject);
    rb_cRactorLock = rb_define_class_under(rb_cRactor, "Lock", rb_cObject);
    rb_cRactorLVar = rb_define_class_under(rb_cRactor, "LVar", rb_cObject);
}
