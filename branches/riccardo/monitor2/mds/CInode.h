// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
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



#ifndef __CINODE_H
#define __CINODE_H

#include "config.h"
#include "include/types.h"
#include "include/lru.h"

#include "mdstypes.h"

#include "CDentry.h"
#include "SimpleLock.h"
#include "FileLock.h"
#include "ScatterLock.h"
#include "Capability.h"


#include <cassert>
#include <list>
#include <vector>
#include <set>
#include <map>
#include <iostream>
using namespace std;

class Context;
class CDentry;
class CDir;
class Message;
class CInode;
class CInodeDiscover;
class MDCache;


ostream& operator<<(ostream& out, CInode& in);


// cached inode wrapper
class CInode : public MDSCacheObject {
 public:
  // -- pins --
  //static const int PIN_REPLICATED =     1;
  static const int PIN_DIR =        2;
  static const int PIN_CAPS =       7;  // client caps
  static const int PIN_AUTHPIN =    8;
  static const int PIN_IMPORTING =  -9;  // importing
  static const int PIN_ANCHORING = 12;
  static const int PIN_UNANCHORING = 13;
  static const int PIN_OPENINGDIR = 14;
  static const int PIN_REMOTEPARENT = 15;
  static const int PIN_BATCHOPENJOURNAL = 16;

  const char *pin_name(int p) {
    switch (p) {
    case PIN_DIR: return "dir";
    case PIN_CAPS: return "caps";
    case PIN_AUTHPIN: return "authpin";
    case PIN_IMPORTING: return "importing";
    case PIN_ANCHORING: return "anchoring";
    case PIN_UNANCHORING: return "unanchoring";
    case PIN_OPENINGDIR: return "openingdir";
    case PIN_REMOTEPARENT: return "remoteparent";
    case PIN_BATCHOPENJOURNAL: return "batchopenjournal";
    default: return generic_pin_name(p);
    }
  }

  // -- state --
  static const int STATE_ROOT =       (1<<2);
  //static const int STATE_DANGLING =   (1<<4);   // delete me when i expire; i have no dentry
  static const int STATE_EXPORTING =  (1<<6);   // on nonauth bystander.
  static const int STATE_ANCHORING =  (1<<7);
  static const int STATE_UNANCHORING = (1<<8);
  static const int STATE_OPENINGDIR = (1<<9);

  // -- waiters --
  static const int WAIT_SLAVEAGREE  = (1<<0);
  static const int WAIT_AUTHPINNABLE = (1<<1);
  static const int WAIT_DIR         = (1<<2);
  static const int WAIT_ANCHORED    = (1<<3);
  static const int WAIT_UNANCHORED  = (1<<4);
  static const int WAIT_CAPS        = (1<<5);
  
  static const int WAIT_AUTHLOCK_OFFSET = 6;
  static const int WAIT_LINKLOCK_OFFSET = 6 + SimpleLock::WAIT_BITS;
  static const int WAIT_DIRFRAGTREELOCK_OFFSET = 6 + 2*SimpleLock::WAIT_BITS;
  static const int WAIT_FILELOCK_OFFSET = 6 + 3*SimpleLock::WAIT_BITS;
  static const int WAIT_DIRLOCK_OFFSET = 6 + 4*SimpleLock::WAIT_BITS;

  static const int WAIT_ANY           = 0xffffffff;

  // misc
  static const int EXPORT_NONCE = 1; // nonce given to replicas created by export

  ostream& print_db_line_prefix(ostream& out);

 public:
  MDCache *mdcache;

  // inode contents proper
  inode_t          inode;        // the inode itself
  string           symlink;      // symlink dest, if symlink
  fragtree_t       dirfragtree;  // dir frag tree, if any
  map<frag_t,int>  dirfrag_size; // size of each dirfrag

  off_t            last_open_journaled;  // log offset for the last journaled EOpen

  // -- cache infrastructure --
  map<frag_t,CDir*> dirfrags; // cached dir fragments

  frag_t pick_dirfrag(const string &dn);
  CDir* get_dirfrag(frag_t fg) {
    if (dirfrags.count(fg)) 
      return dirfrags[fg];
    else
      return 0;
  }
  void get_dirfrags(list<CDir*>& ls);
  void get_nested_dirfrags(list<CDir*>& ls);
  void get_subtree_dirfrags(list<CDir*>& ls);
  CDir *get_or_open_dirfrag(MDCache *mdcache, frag_t fg);
  CDir *add_dirfrag(CDir *dir);
  void close_dirfrag(frag_t fg);
  void close_dirfrags();

 protected:
  // parent dentries in cache
  CDentry         *parent;             // primary link
  set<CDentry*>    remote_parents;     // if hard linked


  // -- distributed state --
protected:
  // file capabilities
  map<int, Capability>  client_caps;         // client -> caps
  map<int, int>         mds_caps_wanted;     // [auth] mds -> caps wanted
  int                   replica_caps_wanted; // [replica] what i've requested from auth
  utime_t               replica_caps_wanted_keep_until;


 private:
  // auth pin
  int auth_pins;
  int nested_auth_pins;

 public:
  meta_load_t popularity[MDS_NPOP];

  // friends
  friend class Server;
  friend class Locker;
  friend class Migrator;
  friend class MDCache;
  friend class CDir;
  friend class CInodeExport;
  friend class CInodeDiscover;

 public:
  // ---------------------------
  CInode(MDCache *c, bool auth=true) : 
    mdcache(c),
    last_open_journaled(0),
    parent(0),
    replica_caps_wanted(0),
    auth_pins(0), nested_auth_pins(0),
    authlock(this, LOCK_OTYPE_IAUTH, WAIT_AUTHLOCK_OFFSET),
    linklock(this, LOCK_OTYPE_ILINK, WAIT_LINKLOCK_OFFSET),
    dirfragtreelock(this, LOCK_OTYPE_IDIRFRAGTREE, WAIT_DIRFRAGTREELOCK_OFFSET),
    filelock(this, LOCK_OTYPE_IFILE, WAIT_FILELOCK_OFFSET),
    dirlock(this, LOCK_OTYPE_IDIR, WAIT_DIRLOCK_OFFSET)
  {
    state = 0;  
    if (auth) state_set(STATE_AUTH);
  };
  ~CInode() {
    close_dirfrags();
  }
  

  // -- accessors --
  bool is_file()    { return inode.is_file(); }
  bool is_symlink() { return inode.is_symlink(); }
  bool is_dir()     { return inode.is_dir(); }

  bool is_anchored() { return inode.anchored; }
  bool is_anchoring() { return state_test(STATE_ANCHORING); }
  bool is_unanchoring() { return state_test(STATE_UNANCHORING); }
  
  bool is_root() { return state & STATE_ROOT; }
  bool is_stray() { return MDS_INO_IS_STRAY(inode.ino); }


  inodeno_t ino() const { return inode.ino; }
  inode_t& get_inode() { return inode; }
  CDentry* get_parent_dn() { return parent; }
  CDir *get_parent_dir();
  CInode *get_parent_inode();
  
  bool is_lt(const MDSCacheObject *r) const {
    return ino() < ((CInode*)r)->ino();
  }



  // -- misc -- 
  void make_path(string& s);
  void make_anchor_trace(vector<class Anchor>& trace);
  void name_stray_dentry(string& dname);


  
  // -- dirtyness --
  version_t get_version() { return inode.version; }

  version_t pre_dirty();
  void _mark_dirty();
  void mark_dirty(version_t projected_dirv);
  void mark_clean();




  CInodeDiscover* replicate_to(int rep);


  // -- waiting --
  void add_waiter(int tag, Context *c);


  // -- locks --
public:
  SimpleLock authlock;
  SimpleLock linklock;
  SimpleLock dirfragtreelock;
  FileLock   filelock;
  ScatterLock dirlock;

  SimpleLock* get_lock(int type) {
    switch (type) {
    case LOCK_OTYPE_IFILE: return &filelock;
    case LOCK_OTYPE_IAUTH: return &authlock;
    case LOCK_OTYPE_ILINK: return &linklock;
    case LOCK_OTYPE_IDIRFRAGTREE: return &dirfragtreelock;
    case LOCK_OTYPE_IDIR: return &dirlock;
    default: assert(0);
    }
  }
  void set_mlock_info(MLock *m);
  void encode_lock_state(int type, bufferlist& bl);
  void decode_lock_state(int type, bufferlist& bl);


  // -- caps -- (new)
  // client caps
  bool is_any_caps() { return !client_caps.empty(); }
  map<int,Capability>& get_client_caps() { return client_caps; }
  void add_client_cap(int client, Capability& cap) {
    if (client_caps.empty())
      get(PIN_CAPS);
    assert(client_caps.count(client) == 0);
    client_caps[client] = cap;
  }
  void remove_client_cap(int client) {
    assert(client_caps.count(client) == 1);
    client_caps.erase(client);
    if (client_caps.empty())
      put(PIN_CAPS);
  }
  Capability* get_client_cap(int client) {
    if (client_caps.count(client))
      return &client_caps[client];
    return 0;
  }
  /*
  void set_client_caps(map<int,Capability>& cl) {
    if (client_caps.empty() && !cl.empty())
      get(PIN_CAPS);
    client_caps.clear();
    client_caps = cl;
  }
  */
  void take_client_caps(map<int,Capability>& cl) {
    if (!client_caps.empty())
      put(PIN_CAPS);
    cl = client_caps;
    client_caps.clear();
  }
  void merge_client_caps(map<int,Capability>& cl, set<int>& new_client_caps) {
    if (client_caps.empty() && !cl.empty())
      get(PIN_CAPS);
    for (map<int,Capability>::iterator it = cl.begin();
         it != cl.end();
         it++) {
      new_client_caps.insert(it->first);
      if (client_caps.count(it->first)) {
        // merge
        client_caps[it->first].merge(it->second);
      } else {
        // new
        client_caps[it->first] = it->second;
      }
    }      
  }

  // caps issued, wanted
  int get_caps_issued() {
    int c = 0;
    for (map<int,Capability>::iterator it = client_caps.begin();
         it != client_caps.end();
         it++) 
      c |= it->second.issued();
    return c;
  }
  int get_caps_wanted() {
    int w = 0;
    for (map<int,Capability>::iterator it = client_caps.begin();
         it != client_caps.end();
         it++) {
      w |= it->second.wanted();
      //cout << " get_caps_wanted client " << it->first << " " << cap_string(it->second.wanted()) << endl;
    }
    if (is_auth())
      for (map<int,int>::iterator it = mds_caps_wanted.begin();
           it != mds_caps_wanted.end();
           it++) {
        w |= it->second;
        //cout << " get_caps_wanted mds " << it->first << " " << cap_string(it->second) << endl;
      }
    return w;
  }


  void replicate_relax_locks() {
    dout(10) << " relaxing locks on " << *this << endl;
    assert(is_auth());
    assert(!is_replicated());

    authlock.replicate_relax();
    linklock.replicate_relax();
    dirfragtreelock.replicate_relax();

    if (get_caps_issued() & (CAP_FILE_WR|CAP_FILE_WRBUFFER) == 0) 
      filelock.replicate_relax();

    dirlock.replicate_relax();
  }


  // -- authority --
  pair<int,int> authority();


  // -- auth pins --
  int is_auth_pinned() { 
    return auth_pins;
  }
  void adjust_nested_auth_pins(int a);
  bool can_auth_pin();
  void auth_pin();
  void auth_unpin();


  // -- freeze --
  bool is_frozen();
  bool is_frozen_dir();
  bool is_freezing();


  // -- reference counting --
  
  /* these can be pinned any # of times, and are
     linked to an active_request, so they're automatically cleaned
     up when a request is finished.  pin at will! */
  void request_pin_get() {
    get(PIN_REQUEST);
  }
  void request_pin_put() {
    put(PIN_REQUEST);
  }

  void bad_put(int by) {
    dout(7) << " bad put " << *this << " by " << by << " " << pin_name(by) << " was " << ref << " (" << ref_set << ")" << endl;
    assert(ref_set.count(by) == 1);
    assert(ref > 0);
  }
  void bad_get(int by) {
    dout(7) << " bad get " << *this << " by " << by << " " << pin_name(by) << " was " << ref << " (" << ref_set << ")" << endl;
    assert(ref_set.count(by) == 0);
  }
  void first_get();
  void last_put();


  // -- hierarchy stuff --
private:
  //void get_parent();
  //void put_parent();

public:
  void set_primary_parent(CDentry *p) {
    assert(parent == 0);
    parent = p;
  }
  void remove_primary_parent(CDentry *dn) {
    assert(dn == parent);
    parent = 0;
  }
  void add_remote_parent(CDentry *p);
  void remove_remote_parent(CDentry *p);
  int num_remote_parents() {
    return remote_parents.size(); 
  }


  /*
  // for giving to clients
  void get_dist_spec(set<int>& ls, int auth, timepair_t& now) {
    if (( is_dir() && popularity[MDS_POP_CURDOM].get(now) > g_conf.mds_bal_replicate_threshold) ||
        (!is_dir() && popularity[MDS_POP_JUSTME].get(now) > g_conf.mds_bal_replicate_threshold)) {
      //if (!cached_by.empty() && inode.ino > 1) dout(1) << "distributed spec for " << *this << endl;
      ls = cached_by;
    }
  }
  */

  void print(ostream& out);

};




// -- encoded state

// discover

class CInodeDiscover {
  
  inode_t    inode;
  string     symlink;
  fragtree_t dirfragtree;

  int        replica_nonce;
  
  int        authlock_state;
  int        linklock_state;
  int        dirfragtreelock_state;
  int        filelock_state;
  int        dirlock_state;

 public:
  CInodeDiscover() {}
  CInodeDiscover(CInode *in, int nonce) {
    inode = in->inode;
    symlink = in->symlink;
    dirfragtree = in->dirfragtree;

    replica_nonce = nonce;

    authlock_state = in->authlock.get_replica_state();
    linklock_state = in->linklock.get_replica_state();
    dirfragtreelock_state = in->dirfragtreelock.get_replica_state();
    filelock_state = in->filelock.get_replica_state();
    dirlock_state = in->dirlock.get_replica_state();
  }

  inodeno_t get_ino() { return inode.ino; }
  int get_replica_nonce() { return replica_nonce; }

  void update_inode(CInode *in) {
    in->inode = inode;
    in->symlink = symlink;
    in->dirfragtree = dirfragtree;

    in->replica_nonce = replica_nonce;
    in->authlock.set_state(authlock_state);
    in->linklock.set_state(linklock_state);
    in->dirfragtreelock.set_state(dirfragtreelock_state);
    in->filelock.set_state(filelock_state);
    in->dirlock.set_state(dirlock_state);
  }
  
  void _encode(bufferlist& bl) {
    ::_encode(inode, bl);
    ::_encode(symlink, bl);
    dirfragtree._encode(bl);
    ::_encode(replica_nonce, bl);
    ::_encode(authlock_state, bl);
    ::_encode(linklock_state, bl);
    ::_encode(dirfragtreelock_state, bl);
    ::_encode(filelock_state, bl);
    ::_encode(dirlock_state, bl);
  }

  void _decode(bufferlist& bl, int& off) {
    ::_decode(inode, bl, off);
    ::_decode(symlink, bl, off);
    dirfragtree._decode(bl, off);
    ::_decode(replica_nonce, bl, off);
    ::_decode(authlock_state, bl, off);
    ::_decode(linklock_state, bl, off);
    ::_decode(dirfragtreelock_state, bl, off);
    ::_decode(filelock_state, bl, off);
    ::_decode(dirlock_state, bl, off);
  }  

};


// export

class CInodeExport {

  struct st_ {
    inode_t        inode;

    meta_load_t    popularity_justme;
    meta_load_t    popularity_curdom;
    bool           is_dirty;       // dirty inode?
    
    int            num_caps;
  } st;

  string         symlink;
  fragtree_t     dirfragtree;

  map<int,int>     replicas;
  map<int,Capability>  cap_map;

  bufferlist locks;

public:
  CInodeExport() {}
  CInodeExport(CInode *in) {
    st.inode = in->inode;
    symlink = in->symlink;
    dirfragtree = in->dirfragtree;

    st.is_dirty = in->is_dirty();
    replicas = in->replicas;

    in->authlock._encode(locks);
    in->linklock._encode(locks);
    in->dirfragtreelock._encode(locks);
    in->filelock._encode(locks);
    in->dirlock._encode(locks);
    
    st.popularity_justme.take( in->popularity[MDS_POP_JUSTME] );
    st.popularity_curdom.take( in->popularity[MDS_POP_CURDOM] );
    in->popularity[MDS_POP_ANYDOM] -= st.popularity_curdom;
    in->popularity[MDS_POP_NESTED] -= st.popularity_curdom;
    
    // steal WRITER caps from inode
    in->take_client_caps(cap_map);
    //remaining_issued = in->get_caps_issued();
  }
  
  inodeno_t get_ino() { return st.inode.ino; }

  void update_inode(CInode *in, set<int>& new_client_caps) {
    // treat scatterlocked mtime special, since replica may have newer info
    if (in->dirlock.get_state() == LOCK_SCATTER ||
	in->dirlock.get_state() == LOCK_GSYNCS)
      st.inode.mtime = MAX(in->inode.mtime, st.inode.mtime);

    in->inode = st.inode;
    in->symlink = symlink;
    in->dirfragtree = dirfragtree;

    in->popularity[MDS_POP_JUSTME] += st.popularity_justme;
    in->popularity[MDS_POP_CURDOM] += st.popularity_curdom;
    in->popularity[MDS_POP_ANYDOM] += st.popularity_curdom;
    in->popularity[MDS_POP_NESTED] += st.popularity_curdom;

    if (st.is_dirty) 
      in->_mark_dirty();

    in->replicas = replicas;
    if (!replicas.empty()) 
      in->get(CInode::PIN_REPLICATED);

    int off = 0;
    in->authlock._decode(locks, off);
    in->linklock._decode(locks, off);
    in->dirfragtreelock._decode(locks, off);
    in->filelock._decode(locks, off);
    in->dirlock._decode(locks, off);

    // caps
    in->merge_client_caps(cap_map, new_client_caps);
  }

  void _encode(bufferlist& bl) {
    st.num_caps = cap_map.size();

    ::_encode(st, bl);
    ::_encode(symlink, bl);
    dirfragtree._encode(bl);
    ::_encode(replicas, bl);
    ::_encode(locks, bl);

    // caps
    for (map<int,Capability>::iterator it = cap_map.begin();
         it != cap_map.end();
         it++) {
      bl.append((char*)&it->first, sizeof(it->first));
      it->second._encode(bl);
    }
  }

  int _decode(bufferlist& bl, int off = 0) {
    ::_decode(st, bl, off);
    ::_decode(symlink, bl, off);
    dirfragtree._decode(bl, off);
    ::_decode(replicas, bl, off);
    ::_decode(locks, bl, off);

    // caps
    for (int i=0; i<st.num_caps; i++) {
      int c;
      bl.copy(off, sizeof(c), (char*)&c);
      off += sizeof(c);
      cap_map[c]._decode(bl, off);
    }

    return off;
  }
};



#endif
