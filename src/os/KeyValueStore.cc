// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 UnitedStack <haomai@unitedstack.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "include/int_types.h"

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <errno.h>
#include <dirent.h>

#include <iostream>
#include <map>

#include "include/compat.h"

#include <fstream>
#include <sstream>

#include "KeyValueStore.h"
#include "common/BackTrace.h"
#include "include/types.h"

#include "osd/osd_types.h"
#include "include/color.h"
#include "include/buffer.h"

#include "common/debug.h"
#include "common/errno.h"
#include "common/run_cmd.h"
#include "common/safe_io.h"
#include "common/perf_counters.h"
#include "common/sync_filesystem.h"
#include "LevelDBStore.h"

#include "common/ceph_crypto.h"
using ceph::crypto::SHA1;

#include "include/assert.h"

#include "common/config.h"

#define dout_subsys ceph_subsys_keyvaluestore

const string KeyValueStore::OBJECT_STRIP_PREFIX = "_STRIP_";
const string KeyValueStore::OBJECT_XATTR = "__OBJATTR__";
const string KeyValueStore::OBJECT_OMAP = "__OBJOMAP__";
const string KeyValueStore::OBJECT_OMAP_HEADER = "__OBJOMAP_HEADER__";
const string KeyValueStore::OBJECT_OMAP_HEADER_KEY = "__OBJOMAP_HEADER__KEY_";
const string KeyValueStore::COLLECTION = "__COLLECTION__";
const string KeyValueStore::COLLECTION_ATTR = "__COLL_ATTR__";

// ============== StripObjectMap Implementation =================

void StripObjectMap::sync_wrap(StripObjectHeader &strip_header,
                               KeyValueDB::Transaction t,
                               const SequencerPosition &spos)
{
  dout(10) << __func__ << " cid: " << strip_header.cid << "oid: "
           << strip_header.oid << " setting spos to " << strip_header.spos
           << dendl;
  strip_header.spos = spos;
  strip_header.header->data.clear();
  ::encode(strip_header, strip_header.header->data);

  sync(strip_header.header, t);
}

bool StripObjectMap::check_spos(const StripObjectHeader &header,
                                const SequencerPosition *spos)
{
  if (!spos || *spos > header.spos) {
    stringstream out;
    if (spos)
      dout(10) << "cid: " << "oid: " << header.oid
               << " not skipping op, *spos " << *spos << dendl;
    else
      dout(10) << "cid: " << "oid: " << header.oid
               << " not skipping op, *spos " << "empty" << dendl;
    dout(10) << " > header.spos " << header.spos << dendl;
    return false;
  } else {
    dout(10) << "cid: " << "oid: " << header.oid << " skipping op, *spos "
             << *spos << " <= header.spos " << header.spos << dendl;
    return true;
  }
}

int StripObjectMap::save_strip_header(StripObjectHeader &strip_header,
                                      KeyValueDB::Transaction t)
{
  strip_header.header->data.clear();
  ::encode(strip_header, strip_header.header->data);

  set_header(strip_header.cid, strip_header.oid, *(strip_header.header), t);
  return 0;
}

int StripObjectMap::create_strip_header(const coll_t &cid,
                                        const ghobject_t &oid,
                                        StripObjectHeader &strip_header,
                                        KeyValueDB::Transaction t)
{
  Header header = lookup_create_header(cid, oid, t);
  if (!header)
    return -EINVAL;

  strip_header.oid = oid;
  strip_header.cid = cid;
  strip_header.header = header;

  return 0;
}

int StripObjectMap::lookup_strip_header(const coll_t &cid,
                                        const ghobject_t &oid,
                                        StripObjectHeader &strip_header)
{
  Header header = lookup_header(cid, oid);

  if (!header) {
    dout(20) << "lookup_strip_header failed to get strip_header "
             << " cid " << cid <<" oid " << oid << dendl;
    return -ENOENT;
  }

  if (header->data.length()) {
    bufferlist::iterator bliter = header->data.begin();
    ::decode(strip_header, bliter);
  }

  if (strip_header.strip_size == 0)
    strip_header.strip_size = default_strip_size;

  strip_header.oid = oid;
  strip_header.cid = cid;
  strip_header.header = header;

  dout(10) << "lookup_strip_header done " << " cid " << cid << " oid "
           << oid << dendl;
  return 0;
}

int StripObjectMap::file_to_extents(uint64_t offset, size_t len,
                                    uint64_t strip_size,
                                    vector<StripExtent> &extents)
{
  if (len == 0)
    return 0;

  uint64_t start, end, strip_offset, extent_offset, extent_len;
  start = offset / strip_size;
  end = (offset + len) / strip_size;
  strip_offset = start * strip_size;

  // "offset" may in the middle of first strip object
  if (offset > strip_offset) {
    extent_offset = offset - strip_offset;
    if (extent_offset + len <= strip_size)
      extent_len = len;
    else
      extent_len = strip_size - extent_offset;
    extents.push_back(StripExtent(start, extent_offset, extent_len));
    start++;
    strip_offset += strip_size;
  }

  for (; start < end; ++start) {
    extents.push_back(StripExtent(start, 0, strip_size));
    strip_offset += strip_size;
  }

  // The end of strip object may be partial
  if (offset + len > strip_offset)
    extents.push_back(StripExtent(start, 0, offset+len-strip_offset));

  assert(extents.size());
  dout(10) << "file_to_extents done " << dendl;
  return 0;
}

void StripObjectMap::clone_wrap(StripObjectHeader &old_header,
                                const coll_t &cid, const ghobject_t &oid,
                                KeyValueDB::Transaction t,
                                const SequencerPosition &spos,
                                StripObjectHeader *origin_header,
                                StripObjectHeader *target_header)
{
  Header new_origin_header;

  if (target_header)
    *target_header = old_header;

  clone(old_header.header, cid, oid, t, &new_origin_header,
        &target_header->header);

  old_header.header = new_origin_header;

  if (origin_header)
    origin_header->spos = spos;

  if (target_header) {
    target_header->oid = oid;
    target_header->cid = cid;
    target_header->spos = spos;
  }
}

void StripObjectMap::rename_wrap(const coll_t &cid, const ghobject_t &oid,
                                 KeyValueDB::Transaction t,
                                 const SequencerPosition &spos,
                                 StripObjectHeader *header)
{
  assert(header);
  rename(header->header, cid, oid, t);

  if (header) {
    header->oid = oid;
    header->cid = cid;
    header->spos = spos;
  }
}


// =========== KeyValueStore::SubmitManager Implementation ==============

uint64_t KeyValueStore::SubmitManager::op_submit_start()
{
  lock.Lock();
  uint64_t op = ++op_seq;
  dout(10) << "op_submit_start " << op << dendl;
  return op;
}

void KeyValueStore::SubmitManager::op_submit_finish(uint64_t op)
{
  dout(10) << "op_submit_finish " << op << dendl;
  if (op != op_submitted + 1) {
      dout(0) << "op_submit_finish " << op << " expected " << (op_submitted + 1)
          << ", OUT OF ORDER" << dendl;
      assert(0 == "out of order op_submit_finish");
  }
  op_submitted = op;
  lock.Unlock();
}


// ========= KeyValueStore::BufferTransaction Implementation ============

int KeyValueStore::BufferTransaction::check_coll(const coll_t &cid)
{
  int r = store->_check_coll(cid);
  if (r == 0)
    return r;

  StripHeaderMap::iterator it = strip_headers.find(
      make_pair(get_coll_for_coll(), make_ghobject_for_coll(cid)));
  if (it != strip_headers.end() && !it->second.deleted) {
    return 0;
  }
  return -ENOENT;
}

int KeyValueStore::BufferTransaction::lookup_cached_header(
    const coll_t &cid, const ghobject_t &oid,
    StripObjectMap::StripObjectHeader **strip_header,
    bool create_if_missing)
{
  if (check_coll(cid) < 0)
    return -ENOENT;

  StripObjectMap::StripObjectHeader header;
  int r = 0;

  StripHeaderMap::iterator it = strip_headers.find(make_pair(cid, oid));
  if (it != strip_headers.end()) {
    if (it->second.deleted)
      return -ENOENT;

    if (strip_header)
      *strip_header = &it->second;
    return 0;
  }

  r = store->backend->lookup_strip_header(cid, oid, header);
  if (r < 0 && create_if_missing) {
    r = store->backend->create_strip_header(cid, oid, header, t);
  }

  if (r < 0) {
    dout(10) << __func__  << " " << cid << "/" << oid << " "
             << " r = " << r << dendl;
    return r;
  }

  strip_headers[make_pair(cid, oid)] = header;
  if (strip_header)
    *strip_header = &strip_headers[make_pair(cid, oid)];
  return r;
}

int KeyValueStore::BufferTransaction::get_buffer_key(
    StripObjectMap::StripObjectHeader *strip_header, const string &prefix,
    const string &key, bufferlist &bl)
{
  if (strip_header->buffers.count(make_pair(prefix, key))) {
    bl.swap(strip_header->buffers[make_pair(prefix, key)]);
    return 0;
  }

  set<string> keys;
  map<string, bufferlist> out;
  keys.insert(key);
  int r = store->backend->get_values(strip_header->cid, strip_header->oid,
                                     prefix, keys, &out);
  if (r < 0) {
    dout(10) << __func__  << " " << strip_header->cid << "/"
             << strip_header->oid << " " << " r = " << r << dendl;
    return r;
  }

  assert(out.size() == 1);
  bl.swap(out.begin()->second);
  return 0;
}

void KeyValueStore::BufferTransaction::set_buffer_keys(
     const string &prefix, StripObjectMap::StripObjectHeader *strip_header,
     map<string, bufferlist> &values)
{
  if (store->backend->check_spos(*strip_header, &spos))
    return ;

  store->backend->set_keys(strip_header->header, prefix, values, t);

  for (map<string, bufferlist>::iterator iter = values.begin();
       iter != values.end(); ++iter) {
    strip_header->buffers[make_pair(prefix, iter->first)].swap(iter->second);
  }
}

int KeyValueStore::BufferTransaction::remove_buffer_keys(
     const string &prefix, StripObjectMap::StripObjectHeader *strip_header,
     const set<string> &keys)
{
  if (store->backend->check_spos(*strip_header, &spos))
    return 0;

  for (set<string>::iterator iter = keys.begin(); iter != keys.end(); ++iter) {
    strip_header->buffers[make_pair(prefix, *iter)] = bufferlist();
  }

  return store->backend->rm_keys(strip_header->header, prefix, keys, t);
}

void KeyValueStore::BufferTransaction::clear_buffer_keys(
     const string &prefix, StripObjectMap::StripObjectHeader *strip_header)
{
  for (map<pair<string, string>, bufferlist>::iterator iter = strip_header->buffers.begin();
       iter != strip_header->buffers.end(); ++iter) {
    if (iter->first.first == prefix)
      iter->second = bufferlist();
  }
}

int KeyValueStore::BufferTransaction::clear_buffer(
     StripObjectMap::StripObjectHeader *strip_header)
{
  if (store->backend->check_spos(*strip_header, &spos))
    return 0;

  strip_header->deleted = true;

  return store->backend->clear(strip_header->header, t);
}

void KeyValueStore::BufferTransaction::clone_buffer(
    StripObjectMap::StripObjectHeader *old_header,
    const coll_t &cid, const ghobject_t &oid)
{
  if (store->backend->check_spos(*old_header, &spos))
    return ;

  // Remove target ahead to avoid dead lock
  strip_headers.erase(make_pair(cid, oid));

  StripObjectMap::StripObjectHeader new_origin_header, new_target_header;

  store->backend->clone_wrap(*old_header, cid, oid, t, spos,
                             &new_origin_header, &new_target_header);

  // FIXME: Lacking of lock for origin header(now become parent), it will
  // cause other operation can get the origin header while submitting
  // transactions
  strip_headers[make_pair(cid, old_header->oid)] = new_origin_header;
  strip_headers[make_pair(cid, oid)] = new_target_header;
}

void KeyValueStore::BufferTransaction::rename_buffer(
    StripObjectMap::StripObjectHeader *old_header,
    const coll_t &cid, const ghobject_t &oid)
{
  if (store->backend->check_spos(*old_header, &spos))
    return ;

  // FIXME: Lacking of lock for origin header, it will cause other operation
  // can get the origin header while submitting transactions
  store->backend->rename_wrap(cid, oid, t, spos, old_header);

  strip_headers.erase(make_pair(old_header->cid, old_header->oid));
  strip_headers[make_pair(cid, oid)] = *old_header;
}

int KeyValueStore::BufferTransaction::submit_transaction()
{
  int r = 0;

  for (StripHeaderMap::iterator header_iter = strip_headers.begin();
       header_iter != strip_headers.end(); ++header_iter) {
    StripObjectMap::StripObjectHeader header = header_iter->second;

    if (store->backend->check_spos(header, &spos))
      continue;

    if (header.deleted)
      continue;

    r = store->backend->save_strip_header(header, t);
    if (r < 0) {
      dout(10) << __func__ << " save strip header failed " << dendl;
      goto out;
    }
  }

out:

  dout(5) << __func__ << " r = " << r << dendl;
  return store->backend->submit_transaction(t);
}

// =========== KeyValueStore Intern Helper Implementation ==============

ostream& operator<<(ostream& out, const KeyValueStore::OpSequencer& s)
{
  assert(&out);
  return out << *s.parent;
}

int KeyValueStore::_create_current()
{
  struct stat st;
  int ret = ::stat(current_fn.c_str(), &st);
  if (ret == 0) {
    // current/ exists
    if (!S_ISDIR(st.st_mode)) {
      dout(0) << "_create_current: current/ exists but is not a directory" << dendl;
      ret = -EINVAL;
    }
  } else {
    ret = ::mkdir(current_fn.c_str(), 0755);
    if (ret < 0) {
      ret = -errno;
      dout(0) << "_create_current: mkdir " << current_fn << " failed: "<< cpp_strerror(ret) << dendl;
    }
  }

  return ret;
}



// =========== KeyValueStore API Implementation ==============

KeyValueStore::KeyValueStore(const std::string &base,
                             const char *name, bool do_update) :
  ObjectStore(base),
  internal_name(name),
  basedir(base),
  fsid_fd(-1), op_fd(-1), current_fd(-1),
  kv_type(KV_TYPE_NONE),
  backend(NULL),
  ondisk_finisher(g_ceph_context),
  lock("KeyValueStore::lock"),
  default_osr("default"),
  op_queue_len(0), op_queue_bytes(0),
  op_finisher(g_ceph_context),
  op_tp(g_ceph_context, "KeyValueStore::op_tp",
        g_conf->filestore_op_threads, "keyvaluestore_op_threads"),
  op_wq(this, g_conf->filestore_op_thread_timeout,
        g_conf->filestore_op_thread_suicide_timeout, &op_tp),
  logger(NULL),
  read_error_lock("KeyValueStore::read_error_lock"),
  m_fail_eio(g_conf->filestore_fail_eio),
  do_update(do_update)
{
  ostringstream oss;
  oss << basedir << "/current";
  current_fn = oss.str();

  ostringstream sss;
  sss << basedir << "/current/commit_op_seq";
  current_op_seq_fn = sss.str();

  // initialize logger
  PerfCountersBuilder plb(g_ceph_context, internal_name, 0, 1);
  logger = plb.create_perf_counters();

  g_ceph_context->get_perfcounters_collection()->add(logger);
  g_ceph_context->_conf->add_observer(this);
}

KeyValueStore::~KeyValueStore()
{
  g_ceph_context->_conf->remove_observer(this);
  g_ceph_context->get_perfcounters_collection()->remove(logger);

  delete logger;
}

int KeyValueStore::statfs(struct statfs *buf)
{
  if (::statfs(basedir.c_str(), buf) < 0) {
    int r = -errno;
    assert(!m_fail_eio || r != -EIO);
    return r;
  }
  return 0;
}

int KeyValueStore::mkfs()
{
  int ret = 0;
  char fsid_fn[PATH_MAX];
  uuid_d old_fsid;

  dout(1) << "mkfs in " << basedir << dendl;

  // open+lock fsid
  snprintf(fsid_fn, sizeof(fsid_fn), "%s/fsid", basedir.c_str());
  fsid_fd = ::open(fsid_fn, O_RDWR|O_CREAT, 0644);
  if (fsid_fd < 0) {
    ret = -errno;
    derr << "mkfs: failed to open " << fsid_fn << ": " << cpp_strerror(ret) << dendl;
    return ret;
  }

  if (lock_fsid() < 0) {
    ret = -EBUSY;
    goto close_fsid_fd;
  }

  if (read_fsid(fsid_fd, &old_fsid) < 0 || old_fsid.is_zero()) {
    if (fsid.is_zero()) {
      fsid.generate_random();
      dout(1) << "mkfs generated fsid " << fsid << dendl;
    } else {
      dout(1) << "mkfs using provided fsid " << fsid << dendl;
    }

    char fsid_str[40];
    fsid.print(fsid_str);
    strcat(fsid_str, "\n");
    ret = ::ftruncate(fsid_fd, 0);
    if (ret < 0) {
      ret = -errno;
      derr << "mkfs: failed to truncate fsid: " << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    ret = safe_write(fsid_fd, fsid_str, strlen(fsid_str));
    if (ret < 0) {
      derr << "mkfs: failed to write fsid: " << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    if (::fsync(fsid_fd) < 0) {
      ret = errno;
      derr << "mkfs: close failed: can't write fsid: "
           << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    dout(10) << "mkfs fsid is " << fsid << dendl;
  } else {
    if (!fsid.is_zero() && fsid != old_fsid) {
      derr << "mkfs on-disk fsid " << old_fsid << " != provided " << fsid << dendl;
      ret = -EINVAL;
      goto close_fsid_fd;
    }
    fsid = old_fsid;
    dout(1) << "mkfs fsid is already set to " << fsid << dendl;
  }

  // version stamp
  ret = write_version_stamp();
  if (ret < 0) {
    derr << "mkfs: write_version_stamp() failed: "
         << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  ret = _create_current();
  if (ret < 0) {
    derr << "mkfs: failed to create current/ " << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  if (_detect_backend()) {
    derr << "KeyValueStore::mkfs error in _detect_backend" << dendl;
    ret = -1;
    goto close_fsid_fd;
  }

  {
    KeyValueDB *store;
    if (kv_type == KV_TYPE_LEVELDB) {
      store = new LevelDBStore(g_ceph_context, current_fn);
    } else {
      derr << "KeyValueStore::mkfs error: unknown backend type" << kv_type << dendl;
      ret = -1;
      goto close_fsid_fd;
    }

    store->init();
    stringstream err;
    if (store->create_and_open(err)) {
      derr << "KeyValueStore::mkfs failed to create keyvaluestore backend: "
           << err.str() << dendl;
      ret = -1;
      delete store;
      goto close_fsid_fd;
    } else {
      delete store;
      dout(1) << "keyvaluestore backend exists/created" << dendl;
    }
  }

  dout(1) << "mkfs done in " << basedir << dendl;
  ret = 0;

 close_fsid_fd:
  TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
  return ret;
}

int KeyValueStore::read_fsid(int fd, uuid_d *uuid)
{
  char fsid_str[40];
  int ret = safe_read(fd, fsid_str, sizeof(fsid_str));
  if (ret < 0)
    return ret;
  if (ret == 8) {
    // old 64-bit fsid... mirror it.
    *(uint64_t*)&uuid->uuid[0] = *(uint64_t*)fsid_str;
    *(uint64_t*)&uuid->uuid[8] = *(uint64_t*)fsid_str;
    return 0;
  }

  if (ret > 36)
    fsid_str[36] = 0;
  if (!uuid->parse(fsid_str))
    return -EINVAL;
  return 0;
}

int KeyValueStore::lock_fsid()
{
  struct flock l;
  memset(&l, 0, sizeof(l));
  l.l_type = F_WRLCK;
  l.l_whence = SEEK_SET;
  l.l_start = 0;
  l.l_len = 0;
  int r = ::fcntl(fsid_fd, F_SETLK, &l);
  if (r < 0) {
    int err = errno;
    dout(0) << "lock_fsid failed to lock " << basedir
            << "/fsid, is another ceph-osd still running? "
            << cpp_strerror(err) << dendl;
    return -err;
  }
  return 0;
}

bool KeyValueStore::test_mount_in_use()
{
  dout(5) << "test_mount basedir " << basedir << dendl;
  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/fsid", basedir.c_str());

  // verify fs isn't in use

  fsid_fd = ::open(fn, O_RDWR, 0644);
  if (fsid_fd < 0)
    return 0;   // no fsid, ok.
  bool inuse = lock_fsid() < 0;
  TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
  return inuse;
}

int KeyValueStore::update_version_stamp()
{
  return write_version_stamp();
}

int KeyValueStore::version_stamp_is_valid(uint32_t *version)
{
  bufferptr bp(PATH_MAX);
  int ret = safe_read_file(basedir.c_str(), "store_version",
      bp.c_str(), bp.length());
  if (ret < 0) {
    if (ret == -ENOENT)
      return 0;
    return ret;
  }
  bufferlist bl;
  bl.push_back(bp);
  bufferlist::iterator i = bl.begin();
  ::decode(*version, i);
  if (*version == target_version)
    return 1;
  else
    return 0;
}

int KeyValueStore::write_version_stamp()
{
  bufferlist bl;
  ::encode(target_version, bl);

  return safe_write_file(basedir.c_str(), "store_version",
      bl.c_str(), bl.length());
}

int KeyValueStore::mount()
{
  int ret;
  char buf[PATH_MAX];

  dout(5) << "basedir " << basedir << dendl;

  // make sure global base dir exists
  if (::access(basedir.c_str(), R_OK | W_OK)) {
    ret = -errno;
    derr << "KeyValueStore::mount: unable to access basedir '" << basedir
         << "': " << cpp_strerror(ret) << dendl;
    goto done;
  }

  // get fsid
  snprintf(buf, sizeof(buf), "%s/fsid", basedir.c_str());
  fsid_fd = ::open(buf, O_RDWR, 0644);
  if (fsid_fd < 0) {
    ret = -errno;
    derr << "KeyValueStore::mount: error opening '" << buf << "': "
         << cpp_strerror(ret) << dendl;
    goto done;
  }

  ret = read_fsid(fsid_fd, &fsid);
  if (ret < 0) {
    derr << "KeyValueStore::mount: error reading fsid_fd: "
         << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  if (lock_fsid() < 0) {
    derr << "KeyValueStore::mount: lock_fsid failed" << dendl;
    ret = -EBUSY;
    goto close_fsid_fd;
  }

  dout(10) << "mount fsid is " << fsid << dendl;

  uint32_t version_stamp;
  ret = version_stamp_is_valid(&version_stamp);
  if (ret < 0) {
    derr << "KeyValueStore::mount : error in version_stamp_is_valid: "
         << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  } else if (ret == 0) {
    if (do_update) {
      derr << "KeyValueStore::mount : stale version stamp detected: "
           << version_stamp << ". Proceeding, do_update "
           << "is set, performing disk format upgrade." << dendl;
    } else {
      ret = -EINVAL;
      derr << "KeyValueStore::mount : stale version stamp " << version_stamp
           << ". Please run the KeyValueStore update script before starting "
           << "the OSD, or set keyvaluestore_update_to to " << target_version
           << dendl;
      goto close_fsid_fd;
    }
  }

  current_fd = ::open(current_fn.c_str(), O_RDONLY);
  if (current_fd < 0) {
    ret = -errno;
    derr << "KeyValueStore::mount: error opening: " << current_fn << ": "
         << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  assert(current_fd >= 0);

  if (_detect_backend()) {
    derr << "KeyValueStore::mount error in _detect_backend" << dendl;
    ret = -1;
    goto close_current_fd;
  }

  {
    KeyValueDB *store;
    if (kv_type == KV_TYPE_LEVELDB) {
      store = new LevelDBStore(g_ceph_context, current_fn);
    } else {
      derr << "KeyValueStore::mount error: unknown backend type" << kv_type
           << dendl;
      ret = -1;
      goto close_current_fd;
    }

    store->init();
    stringstream err;
    if (store->open(err)) {
      derr << "KeyValueStore::mount Error initializing keyvaluestore backend: "
           << err.str() << dendl;
      ret = -1;
      delete store;
      goto close_current_fd;
    }

    StripObjectMap *dbomap = new StripObjectMap(store);
    ret = dbomap->init(do_update);
    if (ret < 0) {
      delete dbomap;
      derr << "Error initializing StripObjectMap: " << ret << dendl;
      goto close_current_fd;
    }
    stringstream err2;

    if (g_conf->filestore_debug_omap_check && !dbomap->check(err2)) {
      derr << err2.str() << dendl;;
      delete dbomap;
      ret = -EINVAL;
      goto close_current_fd;
    }

    backend.reset(dbomap);
  }

  op_tp.start();
  op_finisher.start();
  ondisk_finisher.start();

  // all okay.
  return 0;

close_current_fd:
  TEMP_FAILURE_RETRY(::close(current_fd));
  current_fd = -1;
close_fsid_fd:
  TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
done:
  assert(!m_fail_eio || ret != -EIO);
  return ret;
}

int KeyValueStore::umount()
{
  dout(5) << "umount " << basedir << dendl;

  op_tp.stop();
  op_finisher.stop();
  ondisk_finisher.stop();

  if (fsid_fd >= 0) {
    TEMP_FAILURE_RETRY(::close(fsid_fd));
    fsid_fd = -1;
  }
  if (op_fd >= 0) {
    TEMP_FAILURE_RETRY(::close(op_fd));
    op_fd = -1;
  }
  if (current_fd >= 0) {
    TEMP_FAILURE_RETRY(::close(current_fd));
    current_fd = -1;
  }

  backend.reset();

  // nothing
  return 0;
}

int KeyValueStore::get_max_object_name_length()
{
  lock.Lock();
  int ret = pathconf(basedir.c_str(), _PC_NAME_MAX);
  if (ret < 0) {
    int err = errno;
    lock.Unlock();
    if (err == 0)
      return -EDOM;
    return -err;
  }
  lock.Unlock();
  return ret;
}

int KeyValueStore::queue_transactions(Sequencer *posr, list<Transaction*> &tls,
                                      TrackedOpRef osd_op,
                                      ThreadPool::TPHandle *handle)
{
  Context *onreadable;
  Context *ondisk;
  Context *onreadable_sync;
  ObjectStore::Transaction::collect_contexts(
    tls, &onreadable, &ondisk, &onreadable_sync);

  // set up the sequencer
  OpSequencer *osr;
  if (!posr)
    posr = &default_osr;
  if (posr->p) {
    osr = static_cast<OpSequencer *>(posr->p);
    dout(5) << "queue_transactions existing " << *osr << "/" << osr->parent
            << dendl; //<< " w/ q " << osr->q << dendl;
  } else {
    osr = new OpSequencer;
    osr->parent = posr;
    posr->p = osr;
    dout(5) << "queue_transactions new " << *osr << "/" << osr->parent << dendl;
  }

  Op *o = build_op(tls, ondisk, onreadable, onreadable_sync, osd_op);
  uint64_t op = submit_manager.op_submit_start();
  o->op = op;
  dout(5) << "queue_transactions (trailing journal) " << op << " "
          << tls <<dendl;
  queue_op(osr, o);

  submit_manager.op_submit_finish(op);

  return 0;
}


// ============== KeyValueStore Op Handler =================

KeyValueStore::Op *KeyValueStore::build_op(list<Transaction*>& tls,
        Context *ondisk, Context *onreadable, Context *onreadable_sync,
        TrackedOpRef osd_op)
{
  uint64_t bytes = 0, ops = 0;
  for (list<Transaction*>::iterator p = tls.begin();
       p != tls.end();
       ++p) {
    bytes += (*p)->get_num_bytes();
    ops += (*p)->get_num_ops();
  }

  Op *o = new Op;
  o->start = ceph_clock_now(g_ceph_context);
  o->tls.swap(tls);
  o->ondisk = ondisk;
  o->onreadable = onreadable;
  o->onreadable_sync = onreadable_sync;
  o->ops = ops;
  o->bytes = bytes;
  o->osd_op = osd_op;
  return o;
}

void KeyValueStore::queue_op(OpSequencer *osr, Op *o)
{
  // queue op on sequencer, then queue sequencer for the threadpool,
  // so that regardless of which order the threads pick up the
  // sequencer, the op order will be preserved.

  osr->queue(o);

  dout(5) << "queue_op " << o << " seq " << o->op << " " << *osr << " "
          << o->bytes << " bytes" << "   (queue has " << op_queue_len
          << " ops and " << op_queue_bytes << " bytes)" << dendl;
  op_wq.queue(osr);
}

void KeyValueStore::_do_op(OpSequencer *osr, ThreadPool::TPHandle &handle)
{
  // inject a stall?
  if (g_conf->filestore_inject_stall) {
    int orig = g_conf->filestore_inject_stall;
    dout(5) << "_do_op filestore_inject_stall " << orig << ", sleeping" << dendl;
    for (int n = 0; n < g_conf->filestore_inject_stall; n++)
      sleep(1);
    g_conf->set_val("filestore_inject_stall", "0");
    dout(5) << "_do_op done stalling" << dendl;
  }

  // FIXME: Suppose the collection of transaction only affect objects in the
  // one PG, so this lock will ensure no other concurrent write operation
  osr->apply_lock.Lock();
  Op *o = osr->peek_queue();
  dout(5) << "_do_op " << o << " seq " << o->op << " " << *osr << "/" << osr->parent << " start" << dendl;
  int r = _do_transactions(o->tls, o->op, &handle);
  dout(10) << "_do_op " << o << " seq " << o->op << " r = " << r
           << ", finisher " << o->onreadable << " " << o->onreadable_sync << dendl;

  if (o->ondisk) {
    if (r < 0) {
      delete o->ondisk;
      o->ondisk = 0;
    } else {
      ondisk_finisher.queue(o->ondisk, r);
    }
  }
}

void KeyValueStore::_finish_op(OpSequencer *osr)
{
  Op *o = osr->dequeue();

  dout(10) << "_finish_op " << o << " seq " << o->op << " " << *osr << "/" << osr->parent << dendl;
  osr->apply_lock.Unlock();  // locked in _do_op

  utime_t lat = ceph_clock_now(g_ceph_context);
  lat -= o->start;

  if (o->onreadable_sync) {
    o->onreadable_sync->complete(0);
  }
  op_finisher.queue(o->onreadable);
  delete o;
}

// Combine all the ops in the same transaction using "BufferTransaction" and
// cache the middle results in order to make visible to the following ops.
//
// Lock: KeyValueStore use "in_use" in GenericObjectMap to avoid concurrent
// operation on the same object. Not sure ReadWrite lock should be applied to
// improve concurrent performance. In the future, I'd like to remove apply_lock
// on "osr" and introduce PG RWLock.
int KeyValueStore::_do_transactions(list<Transaction*> &tls, uint64_t op_seq,
  ThreadPool::TPHandle *handle)
{
  int r = 0;

  uint64_t bytes = 0, ops = 0;
  for (list<Transaction*>::iterator p = tls.begin();
       p != tls.end();
       ++p) {
    bytes += (*p)->get_num_bytes();
    ops += (*p)->get_num_ops();
  }

  int trans_num = 0;
  SequencerPosition spos(op_seq, trans_num, 0);
  BufferTransaction bt(this, spos);

  for (list<Transaction*>::iterator p = tls.begin();
       p != tls.end();
       ++p, trans_num++) {
    r = _do_transaction(**p, bt, spos, handle);
    if (r < 0)
      break;
    if (handle)
      handle->reset_tp_timeout();
  }

  r = bt.submit_transaction();
  if (r < 0) {
    assert(0 == "unexpected error");  // FIXME
  }

  return r;
}

unsigned KeyValueStore::_do_transaction(Transaction& transaction,
                                        BufferTransaction &t,
                                        SequencerPosition& spos,
                                        ThreadPool::TPHandle *handle)
{
  dout(10) << "_do_transaction on " << &transaction << dendl;

  Transaction::iterator i = transaction.begin();

  while (i.have_op()) {
    if (handle)
      handle->reset_tp_timeout();

    int op = i.get_op();
    int r = 0;

    switch (op) {
    case Transaction::OP_NOP:
      break;

    case Transaction::OP_TOUCH:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        r = _touch(cid, oid, t);
      }
      break;

    case Transaction::OP_WRITE:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        uint64_t off = i.get_length();
        uint64_t len = i.get_length();
        bool replica = i.get_replica();
        bufferlist bl;
        i.get_bl(bl);
        r = _write(cid, oid, off, len, bl, t, replica);
      }
      break;

    case Transaction::OP_ZERO:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        uint64_t off = i.get_length();
        uint64_t len = i.get_length();
        r = _zero(cid, oid, off, len, t);
      }
      break;

    case Transaction::OP_TRIMCACHE:
      {
        i.get_cid();
        i.get_oid();
        i.get_length();
        i.get_length();
        // deprecated, no-op
      }
      break;

    case Transaction::OP_TRUNCATE:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        uint64_t off = i.get_length();
        r = _truncate(cid, oid, off, t);
      }
      break;

    case Transaction::OP_REMOVE:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        r = _remove(cid, oid, t);
      }
      break;

    case Transaction::OP_SETATTR:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        string name = i.get_attrname();
        bufferlist bl;
        i.get_bl(bl);
        map<string, bufferptr> to_set;
        to_set[name] = bufferptr(bl.c_str(), bl.length());
        r = _setattrs(cid, oid, to_set, t);
        if (r == -ENOSPC)
          dout(0) << " ENOSPC on setxattr on " << cid << "/" << oid
                  << " name " << name << " size " << bl.length() << dendl;
      }
      break;

    case Transaction::OP_SETATTRS:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        map<string, bufferptr> aset;
        i.get_attrset(aset);
        r = _setattrs(cid, oid, aset, t);
        if (r == -ENOSPC)
          dout(0) << " ENOSPC on setxattrs on " << cid << "/" << oid << dendl;
      }
      break;

    case Transaction::OP_RMATTR:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        string name = i.get_attrname();
        r = _rmattr(cid, oid, name.c_str(), t);
      }
      break;

    case Transaction::OP_RMATTRS:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        r = _rmattrs(cid, oid, t);
      }
      break;

    case Transaction::OP_CLONE:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        ghobject_t noid = i.get_oid();
        r = _clone(cid, oid, noid, t);
      }
      break;

    case Transaction::OP_CLONERANGE:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        ghobject_t noid = i.get_oid();
        uint64_t off = i.get_length();
        uint64_t len = i.get_length();
        r = _clone_range(cid, oid, noid, off, len, off, t);
      }
      break;

    case Transaction::OP_CLONERANGE2:
      {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        ghobject_t noid = i.get_oid();
        uint64_t srcoff = i.get_length();
        uint64_t len = i.get_length();
        uint64_t dstoff = i.get_length();
        r = _clone_range(cid, oid, noid, srcoff, len, dstoff, t);
      }
      break;

    case Transaction::OP_MKCOLL:
      {
        coll_t cid = i.get_cid();
        r = _create_collection(cid, t);
      }
      break;

    case Transaction::OP_RMCOLL:
      {
        coll_t cid = i.get_cid();
        r = _destroy_collection(cid, t);
      }
      break;

    case Transaction::OP_COLL_ADD:
      {
        coll_t ncid = i.get_cid();
        coll_t ocid = i.get_cid();
        ghobject_t oid = i.get_oid();
        r = _collection_add(ncid, ocid, oid, t);
      }
      break;

    case Transaction::OP_COLL_REMOVE:
       {
        coll_t cid = i.get_cid();
        ghobject_t oid = i.get_oid();
        r = _remove(cid, oid, t);
       }
      break;

    case Transaction::OP_COLL_MOVE:
      {
        // WARNING: this is deprecated and buggy; only here to replay old journals.
        coll_t ocid = i.get_cid();
        coll_t ncid = i.get_cid();
        ghobject_t oid = i.get_oid();
        r = _collection_add(ocid, ncid, oid, t);
        if (r == 0)
          r = _remove(ocid, oid, t);
      }
      break;

    case Transaction::OP_COLL_MOVE_RENAME:
      {
        coll_t oldcid = i.get_cid();
        ghobject_t oldoid = i.get_oid();
        coll_t newcid = i.get_cid();
        ghobject_t newoid = i.get_oid();
        r = _collection_move_rename(oldcid, oldoid, newcid, newoid, t);
      }
      break;

    case Transaction::OP_COLL_SETATTR:
      {
        coll_t cid = i.get_cid();
        string name = i.get_attrname();
        bufferlist bl;
        i.get_bl(bl);
        r = _collection_setattr(cid, name.c_str(), bl.c_str(), bl.length(), t);
      }
      break;

    case Transaction::OP_COLL_RMATTR:
      {
        coll_t cid = i.get_cid();
        string name = i.get_attrname();
        r = _collection_rmattr(cid, name.c_str(), t);
      }
      break;

    case Transaction::OP_STARTSYNC:
      {
        start_sync();
        break;
      }

    case Transaction::OP_COLL_RENAME:
      {
        coll_t cid(i.get_cid());
        coll_t ncid(i.get_cid());
        r = _collection_rename(cid, ncid, t);
      }
      break;

    case Transaction::OP_OMAP_CLEAR:
      {
        coll_t cid(i.get_cid());
        ghobject_t oid = i.get_oid();
        r = _omap_clear(cid, oid, t);
      }
      break;
    case Transaction::OP_OMAP_SETKEYS:
      {
        coll_t cid(i.get_cid());
        ghobject_t oid = i.get_oid();
        map<string, bufferlist> aset;
        i.get_attrset(aset);
        r = _omap_setkeys(cid, oid, aset, t);
      }
      break;
    case Transaction::OP_OMAP_RMKEYS:
      {
        coll_t cid(i.get_cid());
        ghobject_t oid = i.get_oid();
        set<string> keys;
        i.get_keyset(keys);
        r = _omap_rmkeys(cid, oid, keys, t);
      }
      break;
    case Transaction::OP_OMAP_RMKEYRANGE:
      {
        coll_t cid(i.get_cid());
        ghobject_t oid = i.get_oid();
        string first, last;
        first = i.get_key();
        last = i.get_key();
        r = _omap_rmkeyrange(cid, oid, first, last, t);
      }
      break;
    case Transaction::OP_OMAP_SETHEADER:
      {
        coll_t cid(i.get_cid());
        ghobject_t oid = i.get_oid();
        bufferlist bl;
        i.get_bl(bl);
        r = _omap_setheader(cid, oid, bl, t);
      }
      break;
    case Transaction::OP_SPLIT_COLLECTION:
      {
        coll_t cid(i.get_cid());
        uint32_t bits(i.get_u32());
        uint32_t rem(i.get_u32());
        coll_t dest(i.get_cid());
        r = _split_collection_create(cid, bits, rem, dest, t);
      }
      break;
    case Transaction::OP_SPLIT_COLLECTION2:
      {
        coll_t cid(i.get_cid());
        uint32_t bits(i.get_u32());
        uint32_t rem(i.get_u32());
        coll_t dest(i.get_cid());
        r = _split_collection(cid, bits, rem, dest, t);
      }
      break;

    default:
      derr << "bad op " << op << dendl;
      assert(0);
    }

    if (r < 0) {
      bool ok = false;

      if (r == -ENOENT && !(op == Transaction::OP_CLONERANGE ||
                            op == Transaction::OP_CLONE ||
                            op == Transaction::OP_CLONERANGE2))
        // -ENOENT is normally okay
        // ...including on a replayed OP_RMCOLL with checkpoint mode
        ok = true;
      if (r == -ENODATA)
        ok = true;

      if (!ok) {
        const char *msg = "unexpected error code";

        if (r == -ENOENT && (op == Transaction::OP_CLONERANGE ||
                            op == Transaction::OP_CLONE ||
                            op == Transaction::OP_CLONERANGE2))
          msg = "ENOENT on clone suggests osd bug";

        if (r == -ENOSPC)
          // For now, if we hit _any_ ENOSPC, crash, before we do any damage
          // by partially applying transactions.
          msg = "ENOSPC handling not implemented";

        if (r == -ENOTEMPTY) {
          msg = "ENOTEMPTY suggests garbage data in osd data dir";
        }

        dout(0) << " error " << cpp_strerror(r) << " not handled on operation "
                << op << " (" << spos << ", or op " << spos.op
                << ", counting from 0)" << dendl;
        dout(0) << msg << dendl;
        dout(0) << " transaction dump:\n";
        JSONFormatter f(true);
        f.open_object_section("transaction");
        transaction.dump(&f);
        f.close_section();
        f.flush(*_dout);
        *_dout << dendl;
        assert(0 == "unexpected error");

        if (r == -EMFILE) {
          dump_open_fds(g_ceph_context);
        }
      }
    }

    spos.op++;
  }

  return 0;  // FIXME count errors
}


// =========== KeyValueStore Op Implementation ==============
// objects

int KeyValueStore::_check_coll(const coll_t &cid)
{
  if (is_coll_obj(cid))
    return 0;

  StripObjectMap::StripObjectHeader header;
  int r = backend->lookup_strip_header(get_coll_for_coll(),
                                       make_ghobject_for_coll(cid), header);
  if (r < 0) {
    dout(10) << __func__ << " could not find header r = " << r << dendl;
    return -ENOENT;
  }

  return 0;
}

bool KeyValueStore::exists(coll_t cid, const ghobject_t& oid)
{
  dout(10) << __func__ << "collection: " << cid << " object: " << oid
           << dendl;
  int r;
  StripObjectMap::StripObjectHeader header;

  r = _check_coll(cid);
  if (r < 0) {
    return r;
  }

  r = backend->lookup_strip_header(cid, oid, header);
  if (r < 0) {
    return false;
  }

  return true;
}

int KeyValueStore::stat(coll_t cid, const ghobject_t& oid,
                        struct stat *st, bool allow_eio)
{
  dout(10) << "stat " << cid << "/" << oid << dendl;

  StripObjectMap::StripObjectHeader header;

  int r = _check_coll(cid);
  if (r < 0) {
    return r;
  }

  r = backend->lookup_strip_header(cid, oid, header);
  if (r < 0) {
    dout(10) << "stat " << cid << "/" << oid << "=" << r << dendl;
    return -ENOENT;
  }

  st->st_blocks = header.max_size / header.strip_size;
  if (header.max_size % header.strip_size)
    st->st_blocks++;
  st->st_nlink = 1;
  st->st_size = header.max_size;
  st->st_blksize = header.strip_size;

  return r;
}

int KeyValueStore::_generic_read(coll_t cid, const ghobject_t& oid,
                                 uint64_t offset, size_t len, bufferlist& bl,
                                 bool allow_eio, BufferTransaction *bt)
{
  dout(15) << __func__ << " " << cid << "/" << oid << " " << offset << "~"
           << len << dendl;

  int r;
  StripObjectMap::StripObjectHeader header;

  r = _check_coll(cid);
  if (r < 0) {
    return r;
  }

  // use strip_header buffer
  if (bt) {
    StripObjectMap::StripObjectHeader *cache_header;
    r = bt->lookup_cached_header(cid, oid, &cache_header, false);
    if (r == 0) {
      header = *cache_header;
    }
  } else {
    r = backend->lookup_strip_header(cid, oid, header);
  }

  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << oid << " " << offset << "~"
              << len << " header isn't exist: r = " << r << dendl;
    return r;
  }

  if (header.max_size < offset) {
    r = -EINVAL;
    dout(10) << __func__ << " " << cid << "/" << oid << ")"
             << " offset exceed the length of bl"<< dendl;
    return r;
  }

  if (len == 0)
    len = header.max_size - offset;

  if (offset + len > header.max_size)
    len = header.max_size - offset;

  vector<StripObjectMap::StripExtent> extents;
  StripObjectMap::file_to_extents(offset, len, header.strip_size,
                                  extents);
  map<string, bufferlist> out;
  set<string> keys;

  for (vector<StripObjectMap::StripExtent>::iterator iter = extents.begin();
       iter != extents.end(); ++iter) {
    bufferlist old;
    string key = strip_object_key(iter->no);

    if (bt && header.buffers.count(make_pair(OBJECT_STRIP_PREFIX, key))) {
      assert(header.bits[iter->no]);
      out[key] = header.buffers[make_pair(OBJECT_STRIP_PREFIX, key)];
    } else if (header.bits[iter->no]) {
      keys.insert(key);
    }
  }

  r = backend->get_values(cid, oid, OBJECT_STRIP_PREFIX, keys, &out);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << oid << " " << offset << "~"
             << len << " = " << r << dendl;
    return r;
  }
  if (out.size() != keys.size()) {
    r = -EINVAL;
    dout(10) << __func__ << " " << cid << "/" << oid << " " << offset << "~"
             << len << " get incorrect key/value pairs " << dendl;
    return r;
  }

  uint64_t readed = 0;

  for (vector<StripObjectMap::StripExtent>::iterator iter = extents.begin();
       iter != extents.end(); ++iter) {
    string key = strip_object_key(iter->no);
    if (readed + header.strip_size > header.max_size) {
      if (header.bits[iter->no]) {
        out[key].copy(0, iter->len, bl);
      } else {
        bl.append_zero(iter->len);
      }

      break;
    }

    if (header.bits[iter->no]) {
      bl.append(out[key]);
    } else {
      bl.append_zero(header.strip_size);
    }
    readed += header.strip_size;
  }

  dout(10) << __func__ << " " << cid << "/" << oid << " " << offset
           << "~" << bl.length() << "/" << len << " r = " << r << dendl;

  return bl.length();
}


int KeyValueStore::read(coll_t cid, const ghobject_t& oid, uint64_t offset,
                        size_t len, bufferlist& bl, bool allow_eio)
{
  return _generic_read(cid, oid, offset, len, bl, allow_eio);
}

int KeyValueStore::fiemap(coll_t cid, const ghobject_t& oid,
                          uint64_t offset, size_t len, bufferlist& bl)
{
  dout(10) << __func__ << " " << cid << " " << oid << " " << offset << "~"
           << len << dendl;
  int r;
  StripObjectMap::StripObjectHeader header;

  r = _check_coll(cid);
  if (r < 0) {
    return r;
  }

  r = backend->lookup_strip_header(cid, oid, header);
  if (r < 0) {
    dout(10) << "fiemap " << cid << "/" << oid << " " << offset << "~" << len
             << " failed to get header: r = " << r << dendl;
    return r;
  }

  vector<StripObjectMap::StripExtent> extents;
  StripObjectMap::file_to_extents(offset, len, header.strip_size,
                                  extents);

  map<uint64_t, uint64_t> m;
  for (vector<StripObjectMap::StripExtent>::iterator iter = extents.begin();
       iter != extents.end(); ++iter) {
    m[iter->offset] = iter->len;
  }
  ::encode(m, bl);
  return 0;
}

int KeyValueStore::_remove(coll_t cid, const ghobject_t& oid,
                           BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oid << dendl;

  int r;
  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(cid, oid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << oid << " "
             << " failed to get header: r = " << r << dendl;
    return r;
  }

  r = t.clear_buffer(header);

  dout(10) << __func__ << " " << cid << "/" << oid << " = " << r << dendl;
  return r;
}

int KeyValueStore::_truncate(coll_t cid, const ghobject_t& oid, uint64_t size,
                             BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oid << " size " << size
           << dendl;

  int r;
  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(cid, oid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << oid << " " << size
             << " failed to get header: r = " << r << dendl;
    return r;
  }

  if (header->max_size == size)
    return 0;

  if (header->max_size > size) {
    vector<StripObjectMap::StripExtent> extents;
    StripObjectMap::file_to_extents(size, header->max_size,
                                    header->strip_size, extents);
    assert(extents.size());

    vector<StripObjectMap::StripExtent>::iterator iter = extents.begin();
    if (iter->offset != 0) {
      bufferlist value;
      bufferlist old;
      map<string, bufferlist> values;
      r = t.get_buffer_key(header, OBJECT_STRIP_PREFIX,
                           strip_object_key(iter->no), old);
      if (r < 0) {
        dout(10) << __func__ << " " << cid << "/" << oid << " "
                 << size << " = " << r << dendl;
        return r;
      }

      old.copy(0, iter->offset, value);
      value.append_zero(header->strip_size-iter->offset);
      assert(value.length() == header->strip_size);
      ++iter;

      values[strip_object_key(iter->no)] = value;
      t.set_buffer_keys(OBJECT_STRIP_PREFIX, header, values);
    }

    set<string> keys;
    for (; iter != extents.end(); ++iter) {
      if (header->bits[iter->no]) {
        keys.insert(strip_object_key(iter->no));
        header->bits[iter->no] = 0;
      }
    }
    r = t.remove_buffer_keys(OBJECT_STRIP_PREFIX, header, keys);
    if (r < 0) {
      dout(10) << __func__ << " " << cid << "/" << oid << " "
               << size << " = " << r << dendl;
      return r;
    }
  }

  header->bits.resize(size/header->strip_size+1);
  header->max_size = size;

  dout(10) << __func__ << " " << cid << "/" << oid << " size " << size << " = "
           << r << dendl;
  return r;
}

int KeyValueStore::_touch(coll_t cid, const ghobject_t& oid,
                          BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oid << dendl;

  int r;
  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(cid, oid, &header, true);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << oid << " "
             << " failed to get header: r = " << r << dendl;
    r = -EINVAL;
    return r;
  }

  dout(10) << __func__ << " " << cid << "/" << oid << " = " << r << dendl;
  return r;
}

int KeyValueStore::_write(coll_t cid, const ghobject_t& oid,
                          uint64_t offset, size_t len, const bufferlist& bl,
                          BufferTransaction &t, bool replica)
{
  dout(15) << __func__ << " " << cid << "/" << oid << " " << offset << "~"
           << len << dendl;
  int r;
  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(cid, oid, &header, true);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << oid << " " << offset
             << "~" << len << " failed to get header: r = " << r << dendl;
    return r;
  }

  if (len > bl.length())
    len = bl.length();

  if (len + offset > header->max_size) {
    header->max_size = len + offset;
    header->bits.resize(header->max_size/header->strip_size+1);
  }

  vector<StripObjectMap::StripExtent> extents;
  StripObjectMap::file_to_extents(offset, len, header->strip_size,
                                  extents);
  uint64_t bl_offset = 0;
  map<string, bufferlist> values;
  for (vector<StripObjectMap::StripExtent>::iterator iter = extents.begin();
       iter != extents.end(); ++iter) {
    bufferlist value;
    string key = strip_object_key(iter->no);
    if (header->bits[iter->no]) {
      if (iter->offset == 0 && iter->len == header->strip_size) {
        bl.copy(bl_offset, iter->len, value);
        bl_offset += iter->len;
      } else {
        bufferlist old;
        r = t.get_buffer_key(header, OBJECT_STRIP_PREFIX, key, old);
        if (r < 0) {
          dout(10) << __func__ << " failed to get value " << cid << "/" << oid
                   << " " << offset << "~" << len << " = " << r << dendl;
          return r;
        }

        old.copy(0, iter->offset, value);
        bl.copy(bl_offset, iter->len, value);
        bl_offset += iter->len;

        if (value.length() != header->strip_size)
          old.copy(value.length(), header->strip_size-value.length(), value);
      }
    } else {
      if (iter->offset)
        value.append_zero(iter->offset);
      bl.copy(bl_offset, iter->len, value);
      bl_offset += iter->len;

      if (value.length() < header->strip_size)
        value.append_zero(header->strip_size-value.length());

      header->bits[iter->no] = 1;
    }
    assert(value.length() == header->strip_size);
    values[key].swap(value);
  }
  assert(bl_offset == len);

  t.set_buffer_keys(OBJECT_STRIP_PREFIX, header, values);
  dout(10) << __func__ << " " << cid << "/" << oid << " " << offset << "~" << len
           << " = " << r << dendl;

  return r;
}

int KeyValueStore::_zero(coll_t cid, const ghobject_t& oid, uint64_t offset,
                         size_t len, BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oid << " " << offset << "~" << len << dendl;

  bufferptr bp(len);
  bp.zero();
  bufferlist bl;
  bl.push_back(bp);
  int r = _write(cid, oid, offset, len, bl, t);

  dout(20) << __func__ << " " << cid << "/" << oid << " " << offset << "~"
           << len << " = " << r << dendl;
  return r;
}

int KeyValueStore::_clone(coll_t cid, const ghobject_t& oldoid,
                          const ghobject_t& newoid, BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oldoid << " -> " << cid << "/"
           << newoid << dendl;

  if (oldoid == newoid)
    return 0;

  int r;
  StripObjectMap::StripObjectHeader *old_header;

  r = t.lookup_cached_header(cid, oldoid, &old_header, false);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << oldoid << " -> " << cid << "/"
             << newoid << " = " << r << dendl;
    return r;
  }

  t.clone_buffer(old_header, cid, newoid);

  dout(10) << __func__ << " " << cid << "/" << oldoid << " -> " << cid << "/"
           << newoid << " = " << r << dendl;
  return r;
}

int KeyValueStore::_clone_range(coll_t cid, const ghobject_t& oldoid,
                                const ghobject_t& newoid, uint64_t srcoff,
                                uint64_t len, uint64_t dstoff,
                                BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oldoid << " -> " << cid << "/"
           << newoid << " " << srcoff << "~" << len << " to " << dstoff
           << dendl;

  int r;
  bufferlist bl;

  r = _generic_read(cid, oldoid, srcoff, len, bl, &t);
  if (r < 0)
    goto out;

  r = _write(cid, newoid, dstoff, len, bl, t);

 out:
  dout(10) << __func__ << " " << cid << "/" << oldoid << " -> " << cid << "/"
           << newoid << " " << srcoff << "~" << len << " to " << dstoff
           << " = " << r << dendl;
  return r;
}

// attrs

int KeyValueStore::getattr(coll_t cid, const ghobject_t& oid, const char *name,
                           bufferptr &bp)
{
  dout(15) << __func__ << " " << cid << "/" << oid << " '" << name << "'"
           << dendl;

  int r;
  map<string, bufferlist> got;
  set<string> to_get;

  r = _check_coll(cid);
  if (r < 0) {
    return r;
  }

  to_get.insert(string(name));
  r = backend->get_values(cid, oid, OBJECT_XATTR, to_get, &got);
  if (r < 0 && r != -ENOENT) {
    dout(10) << __func__ << " get_xattrs err r =" << r << dendl;
    goto out;
  }
  if (got.empty()) {
    dout(10) << __func__ << " got.size() is 0" << dendl;
    return -ENODATA;
  }
  bp = bufferptr(got.begin()->second.c_str(),
                 got.begin()->second.length());
  r = 0;

 out:
  dout(10) << __func__ << " " << cid << "/" << oid << " '" << name << "' = "
           << r << dendl;
  return r;
}

int KeyValueStore::getattrs(coll_t cid, const ghobject_t& oid,
                           map<string,bufferptr>& aset, bool user_only)
{
  int r;
  map<string, bufferlist> attr_aset;

  r = _check_coll(cid);
  if (r < 0) {
    return r;
  }

  r = backend->get(cid, oid, OBJECT_XATTR, &attr_aset);
  if (r < 0 && r != -ENOENT) {
    dout(10) << __func__ << " could not get attrs r = " << r << dendl;
    goto out;
  }

  if (r == -ENOENT)
    r = 0;

  for (map<string, bufferlist>::iterator i = attr_aset.begin();
       i != attr_aset.end(); ++i) {
    string key;
    if (user_only) {
      if (i->first[0] != '_')
        continue;
      if (i->first == "_")
        continue;
      key = i->first.substr(1, i->first.size());
    } else {
      key = i->first;
    }
    aset.insert(make_pair(key,
                bufferptr(i->second.c_str(), i->second.length())));
  }

 out:
  dout(10) << __func__ << " " << cid << "/" << oid << " = " << r << dendl;

  return r;
}

int KeyValueStore::_setattrs(coll_t cid, const ghobject_t& oid,
                             map<string, bufferptr>& aset,
                             BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oid << dendl;

  int r;

  StripObjectMap::StripObjectHeader *header;
  map<string, bufferlist> attrs;

  r = t.lookup_cached_header(cid, oid, &header, false);
  if (r < 0)
    goto out;

  for (map<string, bufferptr>::iterator it = aset.begin();
       it != aset.end(); ++it) {
    attrs[it->first].push_back(it->second);
  }

  t.set_buffer_keys(OBJECT_XATTR, header, attrs);

out:
  dout(10) << __func__ << " " << cid << "/" << oid << " = " << r << dendl;
  return r;
}


int KeyValueStore::_rmattr(coll_t cid, const ghobject_t& oid, const char *name,
                           BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oid << " '" << name << "'"
           << dendl;

  int r;
  set<string> to_remove;
  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(cid, oid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " could not find header r = " << r
             << dendl;
    return r;
  }

  to_remove.insert(string(name));
  r = t.remove_buffer_keys(OBJECT_XATTR, header, to_remove);

  dout(10) << __func__ << " " << cid << "/" << oid << " '" << name << "' = "
           << r << dendl;
  return r;
}

int KeyValueStore::_rmattrs(coll_t cid, const ghobject_t& oid,
                            BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << oid << dendl;

  int r;
  set<string> attrs;

  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(cid, oid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " could not find header r = " << r
             << dendl;
    return r;
  }

  r = backend->get_keys(cid, oid, OBJECT_XATTR, &attrs);
  if (r < 0 && r != -ENOENT) {
    dout(10) << __func__ << " could not get attrs r = " << r << dendl;
    assert(!m_fail_eio || r != -EIO);
    return r;
  }

  r = t.remove_buffer_keys(OBJECT_XATTR, header, attrs);
  t.clear_buffer_keys(OBJECT_XATTR, header);

  dout(10) << __func__ <<  " " << cid << "/" << oid << " = " << r << dendl;
  return r;
}

// collection attrs

int KeyValueStore::collection_getattr(coll_t c, const char *name,
                                      void *value, size_t size)
{
  dout(15) << __func__ << " " << c.to_str() << " '" << name << "' len "
           << size << dendl;

  bufferlist bl;
  int r;

  r = collection_getattr(c, name, bl);
  if (r < 0)
      goto out;

  if (bl.length() < size) {
    r = bl.length();
    bl.copy(0, bl.length(), static_cast<char*>(value));
  } else {
    r = size;
    bl.copy(0, size, static_cast<char*>(value));
  }

out:
  dout(10) << __func__ << " " << c.to_str() << " '" << name << "' len "
           << size << " = " << r << dendl;
  return r;
}

int KeyValueStore::collection_getattr(coll_t c, const char *name,
                                      bufferlist& bl)
{
  dout(15) << __func__ << " " << c.to_str() << " '" << name
           << "'" << dendl;

  int r = _check_coll(c);
  if (r < 0) {
    return r;
  }

  set<string> keys;
  map<string, bufferlist> out;
  keys.insert(string(name));

  r = backend->get_values(get_coll_for_coll(), make_ghobject_for_coll(c),
                          COLLECTION_ATTR, keys, &out);
  if (r < 0) {
    dout(10) << __func__ << " could not get key" << string(name) << dendl;
    r = -EINVAL;
  }

  assert(out.size());
  bl.swap(out.begin()->second);

  dout(10) << __func__ << " " << c.to_str() << " '" << name << "' len "
           << bl.length() << " = " << r << dendl;
  return bl.length();
}

int KeyValueStore::collection_getattrs(coll_t cid,
                                       map<string, bufferptr> &aset)
{
  dout(10) << __func__ << " " << cid.to_str() << dendl;

  int r = _check_coll(cid);
  if (r < 0) {
    return r;
  }

  map<string, bufferlist> out;
  set<string> keys;

  for (map<string, bufferptr>::iterator it = aset.begin();
       it != aset.end(); it++) {
      keys.insert(it->first);
  }

  r = backend->get_values(get_coll_for_coll(), make_ghobject_for_coll(cid),
                          COLLECTION_ATTR, keys, &out);
  if (r < 0) {
    dout(10) << __func__ << " could not get keys" << dendl;
    r = -EINVAL;
    goto out;
  }

  for (map<string, bufferlist>::iterator it = out.begin(); it != out.end();
       ++it) {
    bufferptr ptr(it->second.c_str(), it->second.length());
    aset.insert(make_pair(it->first, ptr));
  }

 out:
  dout(10) << __func__ << " " << cid.to_str() << " = " << r << dendl;
  return r;
}

int KeyValueStore::_collection_setattr(coll_t c, const char *name,
                                       const void *value, size_t size,
                                       BufferTransaction &t)
{
  dout(10) << __func__ << " " << c << " '" << name << "' len "
           << size << dendl;

  int r;
  bufferlist bl;
  map<string, bufferlist> out;
  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(get_coll_for_coll(),
                             make_ghobject_for_coll(c),
                             &header, false);
  if (r < 0) {
    dout(10) << __func__ << " could not find header r = " << r << dendl;
    return r;
  }

  bl.append(reinterpret_cast<const char*>(value), size);
  out.insert(make_pair(string(name), bl));

  t.set_buffer_keys(COLLECTION_ATTR, header, out);

  dout(10) << __func__ << " " << c << " '"
           << name << "' len " << size << " = " << r << dendl;
  return r;
}

int KeyValueStore::_collection_rmattr(coll_t c, const char *name,
                                      BufferTransaction &t)
{
  dout(15) << __func__ << " " << c << dendl;

  int r = _check_coll(c);
  if (r < 0) {
    return r;
  }

  bufferlist bl;
  set<string> out;
  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(get_coll_for_coll(),
                             make_ghobject_for_coll(c),
                             &header, false);
  if (r < 0) {
    dout(10) << __func__ << " could not find header r = " << r << dendl;
    return r;
  }

  out.insert(string(name));
  r = t.remove_buffer_keys(COLLECTION_ATTR, header, out);

  dout(10) << __func__ << " " << c << " = " << r << dendl;
  return r;
}

int KeyValueStore::_collection_setattrs(coll_t cid,
                                        map<string,bufferptr>& aset,
                                        BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << dendl;

  map<string, bufferlist> attrs;
  StripObjectMap::StripObjectHeader *header;
  int r = t.lookup_cached_header(get_coll_for_coll(),
                                 make_ghobject_for_coll(cid),
                                 &header, false);
  if (r < 0) {
    dout(10) << __func__ << " could not find header r = " << r << dendl;
    return r;
  }

  for (map<string, bufferptr>::iterator it = aset.begin(); it != aset.end();
       ++it) {
    attrs[it->first].push_back(it->second);
  }

  t.set_buffer_keys(COLLECTION_ATTR, header, attrs);

  dout(10) << __func__ << " " << cid << " = " << r << dendl;
  return r;
}


// collections

int KeyValueStore::_create_collection(coll_t c, BufferTransaction &t)
{
  dout(15) << __func__ << " " << c << dendl;

  int r;
  StripObjectMap::StripObjectHeader *header;
  bufferlist bl;

  r = t.lookup_cached_header(get_coll_for_coll(),
                             make_ghobject_for_coll(c), &header,
                             false);
  if (r == 0) {
    r = -EEXIST;
    return r;
  }

  r = t.lookup_cached_header(get_coll_for_coll(),
                             make_ghobject_for_coll(c), &header,
                             true);

  dout(10) << __func__ << " cid " << c << " r = " << r << dendl;
  return r;
}

int KeyValueStore::_destroy_collection(coll_t c, BufferTransaction &t)
{
  dout(15) << __func__ << " " << c << dendl;

  int r;
  uint64_t modified_object = 0;
  StripObjectMap::StripObjectHeader *header;
  vector<ghobject_t> oids;

  r = t.lookup_cached_header(get_coll_for_coll(), make_ghobject_for_coll(c),
                             &header, false);
  if (r < 0) {
    goto out;
  }

  // All modified objects are marked deleted
  for (BufferTransaction::StripHeaderMap::iterator iter = t.strip_headers.begin();
       iter != t.strip_headers.end(); iter++) {
    // sum the total modified object in this PG
    if (iter->first.first != c)
      continue;

    modified_object++;
    if (!iter->second.deleted) {
      r = -ENOTEMPTY;
      goto out;
    }
  }

  r = backend->list_objects(c, ghobject_t(), modified_object+1, &oids,
                            0);
  // No other object
  if (oids.size() != modified_object && oids.size() != 0) {
    r = -ENOTEMPTY;
    goto out;
  }

  for(vector<ghobject_t>::iterator iter = oids.begin();
      iter != oids.end(); ++iter) {
    if (!t.strip_headers.count(make_pair(c, *iter))) {
      r = -ENOTEMPTY;
      goto out;
    }
  }

  r = t.clear_buffer(header);

out:
  dout(10) << __func__ << " " << c << " = " << r << dendl;
  return r;
}


int KeyValueStore::_collection_add(coll_t c, coll_t oldcid,
                                   const ghobject_t& o,
                                   BufferTransaction &t)
{
  dout(15) << __func__ <<  " " << c << "/" << o << " from " << oldcid << "/"
           << o << dendl;

  bufferlist bl;
  StripObjectMap::StripObjectHeader *header, *old_header;

  int r = t.lookup_cached_header(oldcid, o, &old_header, false);
  if (r < 0) {
    goto out;
  }

  r = t.lookup_cached_header(c, o, &header, false);
  if (r == 0) {
    r = -EEXIST;
    dout(10) << __func__ << " " << c << "/" << o << " from " << oldcid << "/"
             << o << " already exist " << dendl;
    goto out;
  }

  r = _generic_read(oldcid, o, 0, old_header->max_size, bl, &t);
  if (r < 0) {
    r = -EINVAL;
    goto out;
  }

  r = _write(c, o, 0, bl.length(), bl, t);
  if (r < 0) {
    r = -EINVAL;
  }

out:
  dout(10) << __func__ << " " << c << "/" << o << " from " << oldcid << "/"
           << o << " = " << r << dendl;
  return r;
}

int KeyValueStore::_collection_move_rename(coll_t oldcid,
                                           const ghobject_t& oldoid,
                                           coll_t c, const ghobject_t& o,
                                           BufferTransaction &t)
{
  dout(15) << __func__ << " " << c << "/" << o << " from " << oldcid << "/"
           << oldoid << dendl;
  int r;
  StripObjectMap::StripObjectHeader *header;

  r = t.lookup_cached_header(c, o, &header, false);
  if (r == 0) {
    dout(10) << __func__ << " " << oldcid << "/" << oldoid << " -> " << c
             << "/" << o << " = " << r << dendl;
    return -EEXIST;
  }

  r = t.lookup_cached_header(oldcid, oldoid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " " << oldcid << "/" << oldoid << " -> " << c
             << "/" << o << " = " << r << dendl;
    return r;
  }

  t.rename_buffer(header, c, o);

  dout(10) << __func__ << " " << c << "/" << o << " from " << oldcid << "/"
           << oldoid << " = " << r << dendl;
  return r;
}

int KeyValueStore::_collection_remove_recursive(const coll_t &cid,
                                                BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << dendl;

  StripObjectMap::StripObjectHeader *header;

  int r = t.lookup_cached_header(get_coll_for_coll(),
                                 make_ghobject_for_coll(cid),
                                 &header, false);
  if (r < 0) {
    return 0;
  }

  vector<ghobject_t> objects;
  ghobject_t max;
  while (!max.is_max()) {
    r = collection_list_partial(cid, max, 200, 300, 0, &objects, &max);
    if (r < 0)
      return r;

    for (vector<ghobject_t>::iterator i = objects.begin();
         i != objects.end(); ++i) {
      r = _remove(cid, *i, t);

      if (r < 0)
        return r;
    }
  }

  r = t.clear_buffer(header);

  dout(10) << __func__ << " " << cid  << " r = " << r << dendl;
  return 0;
}

int KeyValueStore::_collection_rename(const coll_t &cid, const coll_t &ncid,
                                      BufferTransaction &t)
{
  dout(10) << __func__ << " origin cid " << cid << " new cid " << ncid
           << dendl;
  return -EOPNOTSUPP;
}

int KeyValueStore::list_collections(vector<coll_t>& ls)
{
  dout(10) << __func__ << " " << dendl;

  vector<ghobject_t> oids;
  ghobject_t next;
  backend->list_objects(get_coll_for_coll(), ghobject_t(), 0, &oids, &next);
  assert(next == ghobject_t::get_max());

  for (vector<ghobject_t>::const_iterator iter = oids.begin();
       iter != oids.end(); ++iter) {
    ls.push_back(coll_t(iter->hobj.oid.name));
  }

  return 0;
}

bool KeyValueStore::collection_exists(coll_t c)
{
  dout(10) << __func__ << " " << dendl;

  StripObjectMap::StripObjectHeader header;

  int r = _check_coll(c);
  if (r < 0) {
    return false;
  }
  return true;
}

bool KeyValueStore::collection_empty(coll_t c)
{
  dout(10) << __func__ << " " << dendl;

  int r = _check_coll(c);
  if (r < 0) {
    return false;
  }

  vector<ghobject_t> oids;
  backend->list_objects(c, ghobject_t(), 1, &oids, 0);

  return oids.empty();
}

int KeyValueStore::collection_list_range(coll_t c, ghobject_t start,
                                         ghobject_t end, snapid_t seq,
                                         vector<ghobject_t> *ls)
{
  bool done = false;
  ghobject_t next = start;

  int r = _check_coll(c);
  if (r < 0) {
    return r;
  }

  while (!done) {
    vector<ghobject_t> next_objects;
    r = collection_list_partial(c, next, get_ideal_list_min(),
                                get_ideal_list_max(), seq,
                                &next_objects, &next);
    if (r < 0)
      return r;

    ls->insert(ls->end(), next_objects.begin(), next_objects.end());

    // special case for empty collection
    if (ls->empty()) {
      break;
    }

    while (!ls->empty() && ls->back() >= end) {
      ls->pop_back();
      done = true;
    }

    if (next >= end) {
      done = true;
    }
  }

  return 0;
}

int KeyValueStore::collection_list_partial(coll_t c, ghobject_t start,
                                           int min, int max, snapid_t seq,
                                           vector<ghobject_t> *ls,
                                           ghobject_t *next)
{
  dout(10) << __func__ << " " << c << " start:" << start << " is_max:"
           << start.is_max() << dendl;

  if (min < 0 || max < 0)
      return -EINVAL;

  if (start.is_max())
      return 0;

  return backend->list_objects(c, start, max, ls, next);
}

int KeyValueStore::collection_list(coll_t c, vector<ghobject_t>& ls)
{
  return collection_list_partial(c, ghobject_t(), 0, 0, 0, &ls, 0);
}

int KeyValueStore::collection_version_current(coll_t c, uint32_t *version)
{
  *version = COLLECTION_VERSION;
  if (*version == target_version)
    return 1;
  else
    return 0;
}

// omap

int KeyValueStore::omap_get(coll_t c, const ghobject_t &hoid,
                            bufferlist *header, map<string, bufferlist> *out)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;

  int r = _check_coll(c);
  if (r < 0) {
    return r;
  }

  r = backend->get(c, hoid, OBJECT_OMAP, out);
  if (r < 0 && r != -ENOENT) {
    dout(10) << __func__ << " err r =" << r << dendl;
    return r;
  }

  return omap_get_header(c, hoid, header, false);
}

int KeyValueStore::omap_get_header(coll_t c, const ghobject_t &hoid,
                                   bufferlist *bl, bool allow_eio)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;

  int r = _check_coll(c);
  if (r < 0) {
    return r;
  }

  set<string> keys;
  map<string, bufferlist> got;

  keys.insert(OBJECT_OMAP_HEADER_KEY);
  r = backend->get_values(c, hoid, OBJECT_OMAP_HEADER, keys, &got);
  if (r < 0 && r != -ENOENT) {
    assert(allow_eio || !m_fail_eio || r != -EIO);
    dout(10) << __func__ << " err r =" << r << dendl;
    return r;
  }

  if (got.size()) {
    assert(got.size() == 1);
    bl->swap(got.begin()->second);
  }

  return 0;
}

int KeyValueStore::omap_get_keys(coll_t c, const ghobject_t &hoid, set<string> *keys)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;

  int r = _check_coll(c);
  if (r < 0) {
    return r;
  }

  r = backend->get_keys(c, hoid, OBJECT_OMAP, keys);
  if (r < 0 && r != -ENOENT) {
    assert(!m_fail_eio || r != -EIO);
    return r;
  }
  return 0;
}

int KeyValueStore::omap_get_values(coll_t c, const ghobject_t &hoid,
                                   const set<string> &keys,
                                   map<string, bufferlist> *out)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;

  int r = _check_coll(c);
  if (r < 0) {
    return r;
  }

  r = backend->get_values(c, hoid, OBJECT_OMAP, keys, out);
  if (r < 0 && r != -ENOENT) {
    assert(!m_fail_eio || r != -EIO);
    return r;
  }
  return 0;
}

int KeyValueStore::omap_check_keys(coll_t c, const ghobject_t &hoid,
                                   const set<string> &keys, set<string> *out)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;

  int r = _check_coll(c);
  if (r < 0) {
    return r;
  }

  r = backend->check_keys(c, hoid, OBJECT_OMAP, keys, out);
  if (r < 0 && r != -ENOENT) {
    assert(!m_fail_eio || r != -EIO);
    return r;
  }
  return 0;
}

ObjectMap::ObjectMapIterator KeyValueStore::get_omap_iterator(
    coll_t c, const ghobject_t &hoid)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  return backend->get_iterator(c, hoid, OBJECT_OMAP);
}

int KeyValueStore::_omap_clear(coll_t cid, const ghobject_t &hoid,
                               BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;

  StripObjectMap::StripObjectHeader *header;

  int r = t.lookup_cached_header(cid, hoid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << hoid << " "
             << " failed to get header: r = " << r << dendl;
    return r;
  }

  set<string> keys;
  r = backend->get_keys(cid, hoid, OBJECT_OMAP, &keys);
  if (r < 0 && r != -ENOENT) {
    dout(10) << __func__ << " could not get omap_keys r = " << r << dendl;
    assert(!m_fail_eio || r != -EIO);
    return r;
  }

  r = t.remove_buffer_keys(OBJECT_OMAP, header, keys);
  if (r < 0) {
    dout(10) << __func__ << " could not remove keys r = " << r << dendl;
    return r;
  }

  keys.clear();
  keys.insert(OBJECT_OMAP_HEADER_KEY);
  r = t.remove_buffer_keys(OBJECT_OMAP_HEADER, header, keys);
  if (r < 0) {
    dout(10) << __func__ << " could not remove keys r = " << r << dendl;
    return r;
  }

  t.clear_buffer_keys(OBJECT_OMAP_HEADER, header);

  dout(10) << __func__ << " " << cid << "/" << hoid << " r = " << r << dendl;
  return 0;
}

int KeyValueStore::_omap_setkeys(coll_t cid, const ghobject_t &hoid,
                                 map<string, bufferlist> &aset,
                                 BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;

  StripObjectMap::StripObjectHeader *header;

  int r = t.lookup_cached_header(cid, hoid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << hoid << " "
             << " failed to get header: r = " << r << dendl;
    return r;
  }

  t.set_buffer_keys(OBJECT_OMAP, header, aset);

  return 0;
}

int KeyValueStore::_omap_rmkeys(coll_t cid, const ghobject_t &hoid,
                                const set<string> &keys,
                                BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;

  StripObjectMap::StripObjectHeader *header;

  int r = t.lookup_cached_header(cid, hoid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << hoid << " "
             << " failed to get header: r = " << r << dendl;
    return r;
  }

  r = t.remove_buffer_keys(OBJECT_OMAP, header, keys);

  dout(10) << __func__ << " " << cid << "/" << hoid << " r = " << r << dendl;
  return r;
}

int KeyValueStore::_omap_rmkeyrange(coll_t cid, const ghobject_t &hoid,
                                    const string& first, const string& last,
                                    BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << hoid << " [" << first << ","
           << last << "]" << dendl;

  set<string> keys;
  {
    ObjectMap::ObjectMapIterator iter = get_omap_iterator(cid, hoid);
    if (!iter)
      return -ENOENT;

    for (iter->lower_bound(first); iter->valid() && iter->key() < last;
         iter->next()) {
      keys.insert(iter->key());
    }
  }
  return _omap_rmkeys(cid, hoid, keys, t);
}

int KeyValueStore::_omap_setheader(coll_t cid, const ghobject_t &hoid,
                                   const bufferlist &bl,
                                   BufferTransaction &t)
{
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;

  map<string, bufferlist> sets;
  StripObjectMap::StripObjectHeader *header;

  int r = t.lookup_cached_header(cid, hoid, &header, false);
  if (r < 0) {
    dout(10) << __func__ << " " << cid << "/" << hoid << " "
             << " failed to get header: r = " << r << dendl;
    return r;
  }

  sets[OBJECT_OMAP_HEADER_KEY] = bl;
  t.set_buffer_keys(OBJECT_OMAP_HEADER, header, sets);
  return 0;
}

int KeyValueStore::_split_collection(coll_t cid, uint32_t bits, uint32_t rem,
                                     coll_t dest, BufferTransaction &t)
{
  {
    dout(15) << __func__ << " " << cid << " bits: " << bits << dendl;

    StripObjectMap::StripObjectHeader *header;

    int r = t.lookup_cached_header(get_coll_for_coll(),
                                   make_ghobject_for_coll(cid),
                                   &header, false);
    if (r < 0) {
      dout(2) << __func__ << ": " << cid << " DNE" << dendl;
      return 0;
    }

    r = t.lookup_cached_header(get_coll_for_coll(),
                               make_ghobject_for_coll(dest),
                               &header, false);
    if (r < 0) {
      dout(2) << __func__ << ": " << dest << " DNE" << dendl;
      return 0;
    }

    vector<ghobject_t> objects;
    ghobject_t next, current;
    int move_size = 0;
    while (1) {
      collection_list_partial(cid, current, get_ideal_list_min(),
                              get_ideal_list_max(), 0, &objects, &next);

      dout(20) << __func__ << cid << "objects size: " << objects.size()
              << dendl;

      if (objects.empty())
        break;

      for (vector<ghobject_t>::iterator i = objects.begin();
          i != objects.end(); ++i) {
        if (i->match(bits, rem)) {
          if (_collection_add(dest, cid, *i, t) < 0) {
            return -1;
          }
          _remove(cid, *i, t);
          move_size++;
        }
      }

      objects.clear();
      current = next;
    }

    dout(20) << __func__ << "move" << move_size << " object from " << cid
             << "to " << dest << dendl;
  }

  if (g_conf->filestore_debug_verify_split) {
    vector<ghobject_t> objects;
    ghobject_t next;
    while (1) {
      collection_list_partial(cid, next, get_ideal_list_min(),
                              get_ideal_list_max(), 0, &objects, &next);
      if (objects.empty())
        break;

      for (vector<ghobject_t>::iterator i = objects.begin();
           i != objects.end(); ++i) {
        dout(20) << __func__ << ": " << *i << " still in source "
                 << cid << dendl;
        assert(!i->match(bits, rem));
      }
      objects.clear();
    }

    next = ghobject_t();
    while (1) {
      collection_list_partial(dest, next, get_ideal_list_min(),
                              get_ideal_list_max(), 0, &objects, &next);
      if (objects.empty())
        break;

      for (vector<ghobject_t>::iterator i = objects.begin();
           i != objects.end(); ++i) {
        dout(20) << __func__ << ": " << *i << " now in dest "
                 << *i << dendl;
        assert(i->match(bits, rem));
      }
      objects.clear();
    }
  }
  return 0;
}

const char** KeyValueStore::get_tracked_conf_keys() const
{
  static const char* KEYS[] = {
    "filestore_min_sync_interval",
    "filestore_max_sync_interval",
    "filestore_queue_max_ops",
    "filestore_queue_max_bytes",
    "filestore_queue_committing_max_ops",
    "filestore_queue_committing_max_bytes",
    "filestore_commit_timeout",
    "filestore_dump_file",
    "filestore_kill_at",
    "filestore_fail_eio",
    "filestore_replica_fadvise",
    "filestore_sloppy_crc",
    "filestore_sloppy_crc_block_size",
    NULL
  };
  return KEYS;
}

void KeyValueStore::handle_conf_change(const struct md_config_t *conf,
                                       const std::set <std::string> &changed)
{
}

void KeyValueStore::dump_transactions(list<ObjectStore::Transaction*>& ls, uint64_t seq, OpSequencer *osr)
{
}

// ============== KeyValueStore Debug EIO Injection =================

void KeyValueStore::inject_data_error(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  dout(10) << __func__ << ": init error on " << oid << dendl;
  data_error_set.insert(oid);
}

void KeyValueStore::inject_mdata_error(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  dout(10) << __func__ << ": init error on " << oid << dendl;
  mdata_error_set.insert(oid);
}

void KeyValueStore::debug_obj_on_delete(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  dout(10) << __func__ << ": clear error on " << oid << dendl;
  data_error_set.erase(oid);
  mdata_error_set.erase(oid);
}

bool KeyValueStore::debug_data_eio(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  if (data_error_set.count(oid)) {
    dout(10) << __func__ << ": inject error on " << oid << dendl;
    return true;
  } else {
    return false;
  }
}

bool KeyValueStore::debug_mdata_eio(const ghobject_t &oid) {
  Mutex::Locker l(read_error_lock);
  if (mdata_error_set.count(oid)) {
    dout(10) << __func__ << ": inject error on " << oid << dendl;
    return true;
  } else {
    return false;
  }
}
