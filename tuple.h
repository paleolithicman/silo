#ifndef _NDB_TUPLE_H_
#define _NDB_TUPLE_H_

#include <vector>
#include <string>
#include <utility>
#include <limits>
#include <unordered_map>

#include "amd64.h"
#include "core.h"
#include "counter.h"
#include "macros.h"
#include "varkey.h"
#include "util.h"
#include "static_assert.h"
#include "rcu.h"
#include "thread.h"
#include "spinlock.h"
#include "small_unordered_map.h"
#include "prefetch.h"

// just a debug option to help track down a particular
// race condition
//#define dbtuple_QUEUE_TRACKING

template <template <typename> class Protocol, typename Traits>
  class transaction; // forward decl

/**
 * A dbtuple is the type of value which we stick
 * into underlying (non-transactional) data structures- it
 * also contains the memory of the value
 */
struct dbtuple : private util::noncopyable {
public:
  // trying to save space by putting constraints
  // on node maximums
  typedef uint32_t version_t;
  typedef uint16_t node_size_type;
  typedef uint64_t tid_t;
  typedef uint8_t * record_type;
  typedef const uint8_t * const_record_type;
  typedef size_t size_type;
  typedef std::string string_type;

  static const tid_t MIN_TID = 0;
  static const tid_t MAX_TID = (tid_t) -1;

private:
  static const version_t HDR_LOCKED_MASK = 0x1;

  static const version_t HDR_TYPE_SHIFT = 1;
  static const version_t HDR_TYPE_MASK = 0x1 << HDR_TYPE_SHIFT;

  static const version_t HDR_DELETING_SHIFT = 2;
  static const version_t HDR_DELETING_MASK = 0x1 << HDR_DELETING_SHIFT;

  // enqueued is currently un-used
  static const version_t HDR_ENQUEUED_SHIFT = 3;
  static const version_t HDR_ENQUEUED_MASK = 0x1 << HDR_ENQUEUED_SHIFT;

  static const version_t HDR_LATEST_SHIFT = 4;
  static const version_t HDR_LATEST_MASK = 0x1 << HDR_LATEST_SHIFT;

  static const version_t HDR_VERSION_SHIFT = 5;
  static const version_t HDR_VERSION_MASK = ((version_t)-1) << HDR_VERSION_SHIFT;

public:

  // NB(stephentu): ABA problem happens after some multiple of
  // 2^(NBits(version_t)-5) concurrent modifications- somewhat low probability
  // event, so we let it happen
  //
  // constraints:
  //   * enqueued => !deleted
  //   * deleted  => !enqueued
  //
  // [ locked |  type  | deleted | enqueued | latest | version ]
  // [  0..1  |  1..2  |  2..3   |   3..4   |  4..5  |  5..32  ]
  volatile version_t hdr;

  // uninterpreted TID
  tid_t version;

  // small sizes on purpose
  node_size_type size; // actual size of record (0 implies absent record)
  node_size_type alloc_size; // max size record allowed. is the space
                             // available for the record buf
  // must be last field
  union {
    struct {
      struct dbtuple *next;
      uint8_t value_start[0];
    } big;
    struct {
      uint8_t value_start[0];
    } small;
  } d[0];

private:
  // private ctor/dtor b/c we do some special memory stuff
  // ctors start node off as latest node

  static inline ALWAYS_INLINE node_size_type
  CheckBounds(size_type s)
  {
    INVARIANT(s <= std::numeric_limits<node_size_type>::max());
    return s;
  }

  // creates a "small" type (type 0), with an empty (deleted) value
  dbtuple(bool do_big_type, size_type alloc_size)
    : hdr((do_big_type ? HDR_TYPE_MASK : 0) | HDR_LATEST_MASK),
      version(MIN_TID),
      size(0),
      alloc_size(CheckBounds(alloc_size))
  {
    // each logical node starts with one "deleted" entry at MIN_TID
    // (this is indicated by size = 0)
    INVARIANT(((char *)this) + sizeof(*this) == (char *) &d[0]);
    if (do_big_type)
      d->big.next = 0;
    ++g_evt_dbtuple_creates;
    g_evt_dbtuple_bytes_allocated +=
      (alloc_size + sizeof(dbtuple) +
       (do_big_type ? sizeof(dbtuple *) : 0));
  }

  // creates a "big" type (type 1), with a non-empty value
  dbtuple(tid_t version, const_record_type r,
               size_type size, size_type alloc_size,
               struct dbtuple *next, bool set_latest)
    : hdr(HDR_TYPE_MASK | (set_latest ? HDR_LATEST_MASK : 0)),
      version(version),
      size(CheckBounds(size)),
      alloc_size(CheckBounds(alloc_size))
  {
    INVARIANT(size <= alloc_size);
    d->big.next = next;
    NDB_MEMCPY(&d->big.value_start[0], r, size);
    ++g_evt_dbtuple_creates;
    g_evt_dbtuple_bytes_allocated +=
      (alloc_size + sizeof(dbtuple) + sizeof(next));
  }

  friend class rcu;
  ~dbtuple();

  inline size_t
  base_size() const
  {
    if (is_big_type())
      return sizeof(*this) + sizeof(this);
    return sizeof(*this);
  }

  static event_avg_counter g_evt_avg_dbtuple_stable_version_spins;
  static event_avg_counter g_evt_avg_dbtuple_lock_acquire_spins;
  static event_avg_counter g_evt_avg_dbtuple_read_retries;

public:

  inline void
  prefetch() const
  {
#ifdef TUPLE_PREFETCH
    prefetch_bytes(this, base_size() + alloc_size);
#endif
  }

  // gc_chain() schedules this instance, and all instances
  // reachable from this instance for deletion via RCU.
  void gc_chain();

  inline bool
  is_locked() const
  {
    return IsLocked(hdr);
  }

  static inline bool
  IsLocked(version_t v)
  {
    return v & HDR_LOCKED_MASK;
  }

  inline version_t
  lock()
  {
#ifdef ENABLE_EVENT_COUNTERS
    unsigned long nspins = 0;
#endif
    version_t v = hdr;
    while (IsLocked(v) ||
           !__sync_bool_compare_and_swap(&hdr, v, v | HDR_LOCKED_MASK)) {
      nop_pause();
      v = hdr;
#ifdef ENABLE_EVENT_COUNTERS
      ++nspins;
#endif
    }
    COMPILER_MEMORY_FENCE;
    INVARIANT(IsLocked(hdr));
#ifdef ENABLE_EVENT_COUNTERS
    g_evt_avg_dbtuple_lock_acquire_spins.offer(nspins);
#endif
    return hdr;
  }

  inline void
  unlock()
  {
    version_t v = hdr;
    INVARIANT(IsLocked(v));
    const version_t n = Version(v);
    v &= ~HDR_VERSION_MASK;
    v |= (((n + 1) << HDR_VERSION_SHIFT) & HDR_VERSION_MASK);
    v &= ~HDR_LOCKED_MASK;
    INVARIANT(!IsLocked(v));
    COMPILER_MEMORY_FENCE;
    hdr = v;
  }

  inline bool
  is_big_type() const
  {
    return IsBigType(hdr);
  }

  inline bool
  is_small_type() const
  {
    return !is_big_type();
  }

  static inline bool
  IsBigType(version_t v)
  {
    return v & HDR_TYPE_MASK;
  }

  inline bool
  is_deleting() const
  {
    return IsDeleting(hdr);
  }

  static inline bool
  IsDeleting(version_t v)
  {
    return v & HDR_DELETING_MASK;
  }

  inline void
  mark_deleting()
  {
    // the lock on the latest version guards non-latest versions
    INVARIANT(!is_latest() || is_locked());
    INVARIANT(!is_enqueued());
    INVARIANT(!is_deleting());
    hdr |= HDR_DELETING_MASK;
  }

  inline bool
  is_enqueued() const
  {
    return IsEnqueued(hdr);
  }

  static inline bool
  IsEnqueued(version_t v)
  {
    return v & HDR_ENQUEUED_MASK;
  }

  inline bool
  is_latest() const
  {
    return IsLatest(hdr);
  }

  static inline bool
  IsLatest(version_t v)
  {
    return v & HDR_LATEST_MASK;
  }

  inline void
  set_latest(bool latest)
  {
    INVARIANT(is_locked());
    if (latest)
      hdr |= HDR_LATEST_MASK;
    else
      hdr &= ~HDR_LATEST_MASK;
  }

  static inline version_t
  Version(version_t v)
  {
    return (v & HDR_VERSION_MASK) >> HDR_VERSION_SHIFT;
  }

  inline version_t
  stable_version() const
  {
    version_t v = hdr;
#ifdef ENABLE_EVENT_COUNTERS
    unsigned long nspins = 0;
#endif
    while (IsLocked(v)) {
      nop_pause();
      v = hdr;
#ifdef ENABLE_EVENT_COUNTERS
      ++nspins;
#endif
    }
    COMPILER_MEMORY_FENCE;
#ifdef ENABLE_EVENT_COUNTERS
    g_evt_avg_dbtuple_stable_version_spins.offer(nspins);
#endif
    return v;
  }

  /**
   * returns true if succeeded, false otherwise
   */
  inline bool
  try_stable_version(version_t &v, unsigned int spins) const
  {
    v = hdr;
    while (IsLocked(v) && spins) {
      nop_pause();
      v = hdr;
      spins--;
    }
    COMPILER_MEMORY_FENCE;
    return !IsLocked(v);
  }

  inline version_t
  unstable_version() const
  {
    return hdr;
  }

  inline bool
  check_version(version_t version) const
  {
    COMPILER_MEMORY_FENCE;
    return hdr == version;
  }

  inline struct dbtuple *
  get_next()
  {
    if (is_big_type())
      return d->big.next;
    return NULL;
  }

  inline struct dbtuple *
  get_next(version_t v)
  {
    INVARIANT(IsBigType(v) == IsBigType(hdr));
    if (IsBigType(v))
      return d->big.next;
    return NULL;
  }

  inline const struct dbtuple *
  get_next() const
  {
    return const_cast<dbtuple *>(this)->get_next();
  }

  inline const struct dbtuple *
  get_next(version_t v) const
  {
    return const_cast<dbtuple *>(this)->get_next(v);
  }

  // precondition: only big types can call
  inline void
  set_next(struct dbtuple *next)
  {
    INVARIANT(is_big_type());
    d->big.next = next;
  }

  inline void
  clear_next()
  {
    if (is_big_type())
      d->big.next = NULL;
  }

  inline char *
  get_value_start(version_t v)
  {
    INVARIANT(IsBigType(v) == IsBigType(hdr));
    if (IsBigType(v))
      return (char *) &d->big.value_start[0];
    return (char *) &d->small.value_start[0];
  }

  inline const char *
  get_value_start(version_t v) const
  {
    return const_cast<dbtuple *>(this)->get_value_start(v);
  }

private:

  inline bool
  is_not_behind(tid_t t) const
  {
    return version <= t;
  }

#ifdef ENABLE_EVENT_COUNTERS
  struct scoped_recorder {
    scoped_recorder(unsigned long &n) : n(&n) {}
    ~scoped_recorder()
    {
      g_evt_avg_dbtuple_read_retries.offer(*n);
    }
  private:
    unsigned long *n;
  };
#endif

  bool
  record_at(tid_t t, tid_t &start_t, string_type &r, size_t max_len,
            bool require_latest) const
  {
#ifdef ENABLE_EVENT_COUNTERS
    unsigned long nretries = 0;
    scoped_recorder rec(nretries);
#endif
  retry:
    const version_t v = stable_version();
    const struct dbtuple *p = get_next(v);
    const bool found = is_not_behind(t);
    if (found) {
      if (unlikely(require_latest && !IsLatest(v)))
        return false;
      start_t = version;
      const size_t read_sz = std::min(static_cast<size_t>(size), max_len);
      r.assign(get_value_start(v), read_sz);
    }
    if (unlikely(!check_version(v))) {
#ifdef ENABLE_EVENT_COUNTERS
      ++nretries;
#endif
      goto retry;
    }
    if (found)
      return true;
    if (p)
      return p->record_at(t, start_t, r, max_len, false);
    // NB(stephentu): if we reach the end of a chain then we assume that
    // the record exists as a deleted record.
    //
    // This is safe because we have been very careful to not garbage collect
    // elements along the chain until it is guaranteed that the record
    // is superceded by later record in any consistent read. Therefore,
    // if we reach the end of the chain, then it *must* be the case that
    // the record does not actually exist.
    //
    // Note that MIN_TID is the *wrong* tid to use here given wrap-around- we
    // really should be setting this value to the tid which represents the
    // oldest TID possible in the system. But we currently don't implement
    // wrap around
    start_t = MIN_TID;
    r.clear();
    return true;
  }

  static event_counter g_evt_dbtuple_creates;
  static event_counter g_evt_dbtuple_logical_deletes;
  static event_counter g_evt_dbtuple_physical_deletes;
  static event_counter g_evt_dbtuple_bytes_allocated;
  static event_counter g_evt_dbtuple_bytes_freed;
  static event_counter g_evt_dbtuple_spills;
  static event_counter g_evt_dbtuple_inplace_buf_insufficient;
  static event_counter g_evt_dbtuple_inplace_buf_insufficient_on_spill;
  static event_avg_counter g_evt_avg_record_spill_len;

public:

  /**
   * Read the record at tid t. Returns true if such a record exists, false
   * otherwise (ie the record was GC-ed, or other reasons). On a successful
   * read, the value @ start_t will be stored in r
   *
   * NB(stephentu): calling stable_read() while holding the lock
   * is an error- this will cause deadlock
   */
  inline bool
  stable_read(tid_t t, tid_t &start_t, string_type &r,
              size_t max_len = string_type::npos) const
  {
    INVARIANT(max_len > 0); // otherwise something will probably break
    return record_at(t, start_t, r, max_len, true);
  }

  inline bool
  is_latest_version(tid_t t) const
  {
    return is_latest() && is_not_behind(t);
  }

  bool
  stable_is_latest_version(tid_t t) const
  {
    version_t v = 0;
    if (!try_stable_version(v, 16))
      return false;
    // now v is a stable version
    const bool ret = IsLatest(v) && is_not_behind(t);
    // only check_version() if the answer would be true- otherwise,
    // no point in doing a version check
    if (ret && check_version(v))
      return true;
    else
      // no point in retrying, since we know it will fail (since we had a
      // version change)
      return false;
  }

  inline bool
  latest_value_is_nil() const
  {
    return is_latest() && size == 0;
  }

  inline bool
  stable_latest_value_is_nil() const
  {
    version_t v = 0;
    if (!try_stable_version(v, 16))
      return false;
    const bool ret = IsLatest(v) && size == 0;
    if (ret && check_version(v))
      return true;
    else
      return false;
  }

  typedef std::pair<bool, dbtuple *> write_record_ret;

  /**
   * Always writes the record in the latest (newest) version slot,
   * not asserting whether or not inserting r @ t would violate the
   * sorted order invariant
   *
   * XXX: document return value
   */
  template <typename Transaction>
  write_record_ret
  write_record_at(const Transaction *txn, tid_t t, const_record_type r, size_type sz)
  {
    INVARIANT(is_locked());
    INVARIANT(is_latest());

    const version_t v = unstable_version();

    if (!sz)
      ++g_evt_dbtuple_logical_deletes;

    // try to overwrite this record
    if (likely(txn->can_overwrite_record_tid(version, t))) {
      // see if we have enough space

      if (likely(sz <= alloc_size)) {
        // directly update in place
        version = t;
        size = sz;
        NDB_MEMCPY(get_value_start(v), r, sz);
        return write_record_ret(false, NULL);
      }

      // keep in the chain (it's wasteful, but not incorrect)
      // so that cleanup is easier
      dbtuple * const rep = alloc(t, r, sz, this, true);
      INVARIANT(rep->is_latest());
      set_latest(false);
      ++g_evt_dbtuple_inplace_buf_insufficient;
      return write_record_ret(false, rep);
    }

    // need to spill
    ++g_evt_dbtuple_spills;
    g_evt_avg_record_spill_len.offer(size);

    char * const vstart = get_value_start(v);

    if (IsBigType(v) && sz <= alloc_size) {
      dbtuple * const spill = alloc(version, (const_record_type) vstart, size, d->big.next, false);
      INVARIANT(!spill->is_latest());
      set_next(spill);
      version = t;
      size = sz;
      NDB_MEMCPY(vstart, r, sz);
      return write_record_ret(true, NULL);
    }

    dbtuple * const rep = alloc(t, r, sz, this, true);
    INVARIANT(rep->is_latest());
    set_latest(false);
    ++g_evt_dbtuple_inplace_buf_insufficient_on_spill;
    return write_record_ret(true, rep);
  }

  // NB: we round up allocation sizes because jemalloc will do this
  // internally anyways, so we might as well grab more usable space (really
  // just internal vs external fragmentation)

  static inline dbtuple *
  alloc_first(bool do_big_type, size_type alloc_sz)
  {
    INVARIANT(alloc_sz <= std::numeric_limits<node_size_type>::max());
    const size_t big_type_contrib_sz = do_big_type ? sizeof(dbtuple *) : 0;
    const size_t max_actual_alloc_sz =
      std::numeric_limits<node_size_type>::max() + sizeof(dbtuple) + big_type_contrib_sz;
    const size_t actual_alloc_sz =
      std::min(
          util::round_up<size_t, /* lgbase*/ 4>(sizeof(dbtuple) + big_type_contrib_sz + alloc_sz),
          max_actual_alloc_sz);
    char *p = (char *) malloc(actual_alloc_sz);
    INVARIANT(p);
    INVARIANT((actual_alloc_sz - sizeof(dbtuple) - big_type_contrib_sz) >= alloc_sz);
    return new (p) dbtuple(
        do_big_type,
        actual_alloc_sz - sizeof(dbtuple) - big_type_contrib_sz);
  }

  static inline dbtuple *
  alloc(tid_t version, const_record_type value, size_type sz, struct dbtuple *next, bool set_latest)
  {
    INVARIANT(sz <= std::numeric_limits<node_size_type>::max());
    const size_t max_alloc_sz =
      std::numeric_limits<node_size_type>::max() + sizeof(dbtuple) + sizeof(next);
    const size_t alloc_sz =
      std::min(
          util::round_up<size_t, /* lgbase*/ 4>(sizeof(dbtuple) + sizeof(next) + sz),
          max_alloc_sz);
    char *p = (char *) malloc(alloc_sz);
    INVARIANT(p);
    return new (p) dbtuple(
        version, value, sz,
        alloc_sz - sizeof(dbtuple) - sizeof(next), next, set_latest);
  }

  static void
  deleter(void *p)
  {
    dbtuple * const n = (dbtuple *) p;
    INVARIANT(n->is_deleting());
    INVARIANT(!n->is_locked());
    n->~dbtuple();
    free(n);
  }

  static inline void
  release(dbtuple *n)
  {
    if (unlikely(!n))
      return;
    n->mark_deleting();
    rcu::free_with_fn(n, deleter);
  }

  static inline void
  release_no_rcu(dbtuple *n)
  {
    if (unlikely(!n))
      return;
#ifdef CHECK_INVARIANTS
    n->lock();
    n->mark_deleting();
    n->unlock();
#endif
    n->~dbtuple();
    free(n);
  }

  static std::string
  VersionInfoStr(version_t v);

}
PACKED
;

#endif /* _NDB_TUPLE_H_ */
