// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_MDS_MUTATION_H
#define CEPH_MDS_MUTATION_H

#include "include/interval_set.h"
#include "include/elist.h"
#include "include/filepath.h"

#include "MDSCacheObject.h"
#include "MDSContext.h"

#include "SimpleLock.h"
#include "Capability.h"

#include "common/TrackedOp.h"
#include "messages/MClientRequest.h"
#include "messages/MMDSSlaveRequest.h"
#include "messages/MClientReply.h"

class LogSegment;
class CInode;
class CDir;
class CDentry;
class Session;
class ScatterLock;
struct sr_t;
struct MDLockCache;

struct MutationImpl : public TrackedOp {
  metareqid_t reqid;
  __u32 attempt = 0;      // which attempt for this request
  LogSegment *ls = nullptr;  // the log segment i'm committing to

private:
  utime_t mds_stamp; ///< mds-local timestamp (real time)
  utime_t op_stamp;  ///< op timestamp (client provided)

public:
  // flag mutation as slave
  mds_rank_t slave_to_mds = MDS_RANK_NONE;  // this is a slave request if >= 0.

  // -- my pins and auth_pins --
  struct ObjectState {
    bool pinned = false;
    bool auth_pinned = false;
    mds_rank_t remote_auth_pinned = MDS_RANK_NONE;
  };
  ceph::unordered_map<MDSCacheObject*, ObjectState> object_states;
  int num_pins = 0;
  int num_auth_pins = 0;
  int num_remote_auth_pins = 0;

  const ObjectState* find_object_state(MDSCacheObject *obj) const {
    auto it = object_states.find(obj);
    return it != object_states.end() ? &it->second : nullptr;
  }

  bool is_any_remote_auth_pin() const { return num_remote_auth_pins > 0; }

  // cache pins (so things don't expire)
  CInode* stickydiri = nullptr;

  
  // held locks
  struct LockOp {
    enum {
      RDLOCK		= 1,
      WRLOCK		= 2,
      XLOCK		= 4,
      REMOTE_WRLOCK	= 8,
      STATE_PIN		= 16, // no RW after locked, just pin lock state
    };
    SimpleLock* lock;
    mutable unsigned flags;
    mutable mds_rank_t wrlock_target;
    LockOp(SimpleLock *l, unsigned f=0, mds_rank_t t=MDS_RANK_NONE) :
      lock(l), flags(f), wrlock_target(t) {}
    bool is_rdlock() const { return !!(flags & RDLOCK); }
    bool is_xlock() const { return !!(flags & XLOCK); }
    bool is_wrlock() const { return !!(flags & WRLOCK); }
    void clear_wrlock() const { flags &= ~WRLOCK; }
    bool is_remote_wrlock() const { return !!(flags & REMOTE_WRLOCK); }
    void clear_remote_wrlock() const {
      flags &= ~REMOTE_WRLOCK;
      wrlock_target = MDS_RANK_NONE;
    }
    bool is_state_pin() const { return !!(flags & STATE_PIN); }
    bool operator<(const LockOp& r) const {
      return lock < r.lock;
    }
  };

  struct LockOpVec : public vector<LockOp> {
    void add_rdlock(SimpleLock *lock) {
      emplace_back(lock, LockOp::RDLOCK);
    }
    void erase_rdlock(SimpleLock *lock);
    void add_xlock(SimpleLock *lock, int idx=-1) {
      if (idx >= 0)
	emplace(cbegin() + idx, lock, LockOp::XLOCK);
      else
	emplace_back(lock, LockOp::XLOCK);
    }
    void add_wrlock(SimpleLock *lock, int idx=-1) {
      if (idx >= 0)
	emplace(cbegin() + idx, lock, LockOp::WRLOCK);
      else
	emplace_back(lock, LockOp::WRLOCK);
    }
    void add_remote_wrlock(SimpleLock *lock, mds_rank_t rank) {
      ceph_assert(rank != MDS_RANK_NONE);
      emplace_back(lock, LockOp::REMOTE_WRLOCK, rank);
    }
    void lock_scatter_gather(SimpleLock *lock) {
      emplace_back(lock, LockOp::WRLOCK | LockOp::STATE_PIN);
    }
    void sort_and_merge();

    LockOpVec() {
      reserve(32);
    }
  };
  using lock_set = set<LockOp>;
  using lock_iterator = lock_set::iterator;
  lock_set locks;  // full ordering

  MDLockCache* lock_cache = nullptr;

  lock_iterator emplace_lock(SimpleLock *l, unsigned f=0, mds_rank_t t=MDS_RANK_NONE) {
    last_locked = l;
    return locks.emplace(l, f, t).first;
  }

  bool is_rdlocked(SimpleLock *lock) const;
  bool is_wrlocked(SimpleLock *lock) const;
  bool is_xlocked(SimpleLock *lock) const {
    auto it = locks.find(lock);
    return it != locks.end() && it->is_xlock();
  }
  bool is_remote_wrlocked(SimpleLock *lock) const {
    auto it = locks.find(lock);
    return it != locks.end() && it->is_remote_wrlock();
  }
  bool is_last_locked(SimpleLock *lock) const {
    return lock == last_locked;
  }

  SimpleLock *last_locked = nullptr;
  // lock we are currently trying to acquire.  if we give up for some reason,
  // be sure to eval() this.
  SimpleLock *locking = nullptr;
  mds_rank_t locking_target_mds = -1;

  // if this flag is set, do not attempt to acquire further locks.
  //  (useful for wrlock, which may be a moving auth target)
  enum {
    SNAP_LOCKED		= 1,
    SNAP2_LOCKED	= 2,
    PATH_LOCKED		= 4,
    ALL_LOCKED		= 8,
  };
  int locking_state = 0;

  bool committing = false;
  bool aborted = false;
  bool killed = false;

  // for applying projected inode changes
  list<CInode*> projected_inodes;
  std::vector<CDir*> projected_fnodes;
  list<ScatterLock*> updated_locks;

  list<CInode*> dirty_cow_inodes;
  list<pair<CDentry*,version_t> > dirty_cow_dentries;

  // keep our default values synced with MDRequestParam's
  MutationImpl() : TrackedOp(nullptr, utime_t()) {}
  MutationImpl(OpTracker *tracker, utime_t initiated,
	       const metareqid_t &ri, __u32 att=0, mds_rank_t slave_to=MDS_RANK_NONE)
    : TrackedOp(tracker, initiated),
      reqid(ri), attempt(att),
      slave_to_mds(slave_to) { }
  ~MutationImpl() override {
    ceph_assert(!locking);
    ceph_assert(!lock_cache);
    ceph_assert(num_pins == 0);
    ceph_assert(num_auth_pins == 0);
  }

  bool is_master() const { return slave_to_mds == MDS_RANK_NONE; }
  bool is_slave() const { return slave_to_mds != MDS_RANK_NONE; }

  client_t get_client() const {
    if (reqid.name.is_client())
      return client_t(reqid.name.num());
    return -1;
  }

  void set_mds_stamp(utime_t t) {
    mds_stamp = t;
  }
  utime_t get_mds_stamp() const {
    return mds_stamp;
  }
  void set_op_stamp(utime_t t) {
    op_stamp = t;
  }
  utime_t get_op_stamp() const {
    if (op_stamp != utime_t())
      return op_stamp;
    return get_mds_stamp();
  }

  // pin items in cache
  void pin(MDSCacheObject *object);
  void unpin(MDSCacheObject *object);
  void set_stickydirs(CInode *in);
  void put_stickydirs();
  void drop_pins();

  void start_locking(SimpleLock *lock, int target=-1);
  void finish_locking(SimpleLock *lock);

  // auth pins
  bool is_auth_pinned(MDSCacheObject *object) const;
  void auth_pin(MDSCacheObject *object);
  void auth_unpin(MDSCacheObject *object);
  void drop_local_auth_pins();
  void set_remote_auth_pinned(MDSCacheObject* object, mds_rank_t from);
  void _clear_remote_auth_pinned(ObjectState& stat);

  void add_projected_inode(CInode *in);
  void pop_and_dirty_projected_inodes();
  void add_projected_fnode(CDir *dir);
  void pop_and_dirty_projected_fnodes();
  void add_updated_lock(ScatterLock *lock);
  void add_cow_inode(CInode *in);
  void add_cow_dentry(CDentry *dn);
  void apply();
  void cleanup();

  virtual void print(ostream &out) const {
    out << "mutation(" << this << ")";
  }

  virtual void dump(Formatter *f) const {}
  void _dump_op_descriptor_unlocked(ostream& stream) const override;
};

inline ostream& operator<<(ostream &out, const MutationImpl &mut)
{
  mut.print(out);
  return out;
}

typedef boost::intrusive_ptr<MutationImpl> MutationRef;



/**
 * MDRequestImpl: state we track for requests we are currently processing.
 * mostly information about locks held, so that we can drop them all
 * the request is finished or forwarded.  see request_*().
 */
struct MDRequestImpl : public MutationImpl {
  Session *session;
  elist<MDRequestImpl*>::item item_session_request;  // if not on list, op is aborted.

  // -- i am a client (master) request
  cref_t<MClientRequest> client_request; // client request (if any)

  // tree and depth info of path1 and path2
  inodeno_t dir_root[2] = {0, 0};
  int dir_depth[2] = {-1, -1};
  file_layout_t dir_layout;

  // store up to two sets of dn vectors, inode pointers, for request path1 and path2.
  vector<CDentry*> dn[2];
  CInode *in[2];
  CDentry *straydn;
  snapid_t snapid;

  CInode *tracei;
  CDentry *tracedn;

  inodeno_t alloc_ino, used_prealloc_ino;  
  interval_set<inodeno_t> prealloc_inos;

  int snap_caps = 0;
  int getattr_caps = 0;		///< caps requested by getattr
  bool no_early_reply = false;
  bool did_early_reply = false;
  bool o_trunc = false;		///< request is an O_TRUNC mutation
  bool has_completed = false;	///< request has already completed

  bufferlist reply_extra_bl;

  // inos we did a embedded cap release on, and may need to eval if we haven't since reissued
  map<vinodeno_t, ceph_seq_t> cap_releases;  

  // -- i am a slave request
  cref_t<MMDSSlaveRequest> slave_request; // slave request (if one is pending; implies slave == true)

  // -- i am an internal op
  int internal_op;
  Context *internal_op_finish;
  void *internal_op_private;

  // indicates how may retries of request have been made
  int retry;

  bool is_batch_head = false;

  // indicator for vxattr osdmap update
  bool waited_for_osdmap;

  // break rarely-used fields into a separately allocated structure 
  // to save memory for most ops
  struct More {
    int slave_error = 0;
    set<mds_rank_t> slaves;           // mds nodes that have slave requests to me (implies client_request)
    set<mds_rank_t> waiting_on_slave; // peers i'm waiting for slavereq replies from. 

    // for rename/link/unlink
    set<mds_rank_t> witnessed;       // nodes who have journaled a RenamePrepare
    map<MDSCacheObject*,version_t> pvmap;

    bool has_journaled_slaves = false;
    bool slave_update_journaled = false;
    bool slave_rolling_back = false;
    
    // for rename
    set<mds_rank_t> extra_witnesses; // replica list from srcdn auth (rename)
    mds_rank_t srcdn_auth_mds = MDS_RANK_NONE;
    bufferlist inode_import;
    version_t inode_import_v = 0;
    CInode* rename_inode = nullptr;
    bool is_freeze_authpin = false;
    bool is_ambiguous_auth = false;
    bool is_remote_frozen_authpin = false;
    bool is_inode_exporter = false;

    map<client_t, pair<Session*, uint64_t> > imported_session_map;
    map<CInode*, map<client_t,Capability::Export> > cap_imports;
    
    // for lock/flock
    bool flock_was_waiting = false;

    // for snaps
    version_t stid = 0;
    bufferlist snapidbl;

    sr_t *srci_srnode = nullptr;
    sr_t *desti_srnode = nullptr;

    // called when slave commits or aborts
    Context *slave_commit = nullptr;
    bufferlist rollback_bl;

    MDSContext::vec waiting_for_finish;

    // export & fragment
    CDir* export_dir = nullptr;
    dirfrag_t fragment_base;

    // for internal ops doing lookup
    filepath filepath1;
    filepath filepath2;

    More() {}
  } *_more;


  // ---------------------------------------------------
  struct Params {
    metareqid_t reqid;
    __u32 attempt;
    cref_t<MClientRequest> client_req;
    cref_t<Message> triggering_slave_req;
    mds_rank_t slave_to;
    utime_t initiated;
    utime_t throttled, all_read, dispatched;
    int internal_op;
    // keep these default values synced to MutationImpl's
    Params() : attempt(0), slave_to(MDS_RANK_NONE), internal_op(-1) {}
    const utime_t& get_recv_stamp() const {
      return initiated;
    }
    const utime_t& get_throttle_stamp() const {
      return throttled;
    }
    const utime_t& get_recv_complete_stamp() const {
      return all_read;
    }
    const utime_t& get_dispatch_stamp() const {
      return dispatched;
    }
  };
  MDRequestImpl(const Params* params, OpTracker *tracker) :
    MutationImpl(tracker, params->initiated,
		 params->reqid, params->attempt, params->slave_to),
    session(NULL), item_session_request(this),
    client_request(params->client_req), straydn(NULL), snapid(CEPH_NOSNAP),
    tracei(NULL), tracedn(NULL), alloc_ino(0), used_prealloc_ino(0),
    internal_op(params->internal_op), internal_op_finish(NULL),
    internal_op_private(NULL),
    retry(0),
    waited_for_osdmap(false), _more(NULL) {
    in[0] = in[1] = NULL;
  }
  ~MDRequestImpl() override;
  
  More* more();
  bool has_more() const;
  bool has_witnesses();
  bool slave_did_prepare();
  bool slave_rolling_back();
  bool did_ino_allocation() const;
  bool freeze_auth_pin(CInode *inode);
  void unfreeze_auth_pin(bool clear_inode=false);
  void set_remote_frozen_auth_pin(CInode *inode);
  bool can_auth_pin(MDSCacheObject *object);
  void drop_local_auth_pins();
  void set_ambiguous_auth(CInode *inode);
  void clear_ambiguous_auth();
  const filepath& get_filepath();
  const filepath& get_filepath2();
  void set_filepath(const filepath& fp);
  void set_filepath2(const filepath& fp);
  bool is_queued_for_replay() const;
  bool is_batch_op();
  int compare_paths();

  void print(ostream &out) const override;
  void dump(Formatter *f) const override;

  cref_t<MClientRequest> release_client_request();
  void reset_slave_request(const cref_t<MMDSSlaveRequest>& req=nullptr);

  // TrackedOp stuff
  typedef boost::intrusive_ptr<MDRequestImpl> Ref;
  std::vector<Ref> batch_reqs;
protected:
  void _dump(Formatter *f) const override;
  void _dump_op_descriptor_unlocked(ostream& stream) const override;
private:
  mutable ceph::spinlock msg_lock;
};

typedef boost::intrusive_ptr<MDRequestImpl> MDRequestRef;


struct MDSlaveUpdate {
  int origop;
  bufferlist rollback;
  elist<MDSlaveUpdate*>::item item;
  Context *waiter;
  set<CInode*> olddirs;
  set<CInode*> unlinked;
  MDSlaveUpdate(int oo, bufferlist &rbl, elist<MDSlaveUpdate*> &list) :
    origop(oo),
    item(this),
    waiter(0) {
    rollback.claim(rbl);
    list.push_back(&item);
  }
  ~MDSlaveUpdate() {
    item.remove_myself();
    if (waiter)
      waiter->complete(0);
  }
};

struct MDLockCacheItem {
  MDLockCache *parent = nullptr;
  elist<MDLockCacheItem*>::item item_lock;
};

struct MDLockCache : public MutationImpl {
  CInode *diri;
  Capability *client_cap;
  int opcode;

  elist<MDLockCache*>::item item_cap_lock_cache;

  using LockItem = MDLockCacheItem;
  // link myself to locked locks
  std::unique_ptr<LockItem[]> items_lock;

  struct DirItem {
    MDLockCache *parent = nullptr;
    elist<DirItem*>::item item_dir;
  };
  // link myself to auth-pinned dirfrags
  std::unique_ptr<DirItem[]> items_dir;
  std::vector<CDir*> auth_pinned_dirfrags;

  int ref = 1;
  bool invalidating = false;

  MDLockCache(Capability *cap, int op) :
    MutationImpl(), diri(cap->get_inode()), client_cap(cap), opcode(op) {
    client_cap->lock_caches.push_back(&item_cap_lock_cache);
  }

  CInode *get_dir_inode() { return diri; }
  void attach_locks();
  void attach_dirfrags(std::vector<CDir*>&& dfv);
  void detach_all();
};

#endif
