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


#include "ClassMonitor.h"
#include "Monitor.h"
#include "MonitorStore.h"

#include "messages/MMonCommand.h"
#include "messages/MClass.h"
#include "messages/MClassAck.h"

#include "common/Timer.h"

#include "osd/osd_types.h"
#include "osd/PG.h"  // yuck

#include "config.h"
#include <sstream>

#define DOUT_SUBSYS mon
#undef dout_prefix
#define dout_prefix _prefix(mon, paxos->get_version())
static ostream& _prefix(Monitor *mon, version_t v) {
  return *_dout << dbeginl
		<< "mon" << mon->whoami
		<< (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)")))
		<< ".class v" << v << " ";
}

ostream& operator<<(ostream& out, ClassMonitor& pm)
{
  std::stringstream ss;
 
  return out << "class";
}

/*
 Tick function to update the map based on performance every N seconds
*/

void ClassMonitor::tick() 
{
  if (!paxos->is_active()) return;

  update_from_paxos();
  dout(10) << *this << dendl;

  if (!mon->is_leader()) return; 

}

void ClassMonitor::create_initial(bufferlist& bl)
{
  dout(10) << "create_initial -- creating initial map" << dendl;
  ClassImpl i;
  ClassLibrary l;
  l.name = "test";
  l.version = 12;
  i.seq = 0;
  i.stamp = g_clock.now();
  bufferptr ptr(1024);
  memset(ptr.c_str(), 0x13, 1024);
  i.binary.append(ptr);
  ClassLibraryIncremental inc;
  ::encode(i, inc.impl);
  ::encode(l, inc.info);
  inc.add = true;

  pending_class.insert(pair<utime_t,ClassLibraryIncremental>(i.stamp, inc));
}

bool ClassMonitor::update_from_paxos()
{
  version_t paxosv = paxos->get_version();

  if (paxosv == list.version) return true;
  assert(paxosv >= list.version);

  dout(0) << "ClassMonitor::update_from_paxos() paxosv=" << paxosv << " list.version=" << list.version << dendl;

  bufferlist blog;

  if (list.version == 0 && paxosv > 1) {
    // startup: just load latest full map
    bufferlist latest;
    version_t v = paxos->get_latest(latest);
    if (v) {
      dout(7) << "update_from_paxos startup: loading summary e" << v << dendl;
      bufferlist::iterator p = latest.begin();
      ::decode(list, p);
    }
  } 

  // walk through incrementals
  while (paxosv > list.version) {
    bufferlist bl;
    bool success = paxos->read(list.version+1, bl);
    assert(success);

    bufferlist::iterator p = bl.begin();
    ClassLibraryIncremental inc;
    ::decode(inc, p);
    ClassImpl impl;
    ClassLibrary info;
    inc.decode_impl(impl);
    inc.decode_info(info);
    if (inc.add) {
      char *store_name;
      int len = info.name.length() + 16;
      store_name = (char *)malloc(len);
      snprintf(store_name, len, "%s.%d", info.name.c_str(), (int)info.version);
      dout(0) << "storing inc.impl length=" << inc.impl.length() << dendl;
      mon->store->put_bl_ss(impl.binary, "class_impl", store_name);
      mon->store->append_bl_ss(inc.info, "class_impl", store_name);
      dout(0) << "adding name=" << info.name << " version=" << info.version <<  " store_name=" << store_name << dendl;
      free(store_name);
      list.add(info.name, info.version);
    } else {
      list.remove(info.name, info.version);
    }

    list.version++;
  }

  bufferlist bl;
  ::encode(list, bl);
  paxos->stash_latest(paxosv, bl);

  return true;
}

void ClassMonitor::create_pending()
{
  pending_class.clear();
  pending_list = list;
  dout(10) << "create_pending v " << (paxos->get_version() + 1) << dendl;
}

void ClassMonitor::encode_pending(bufferlist &bl)
{
  dout(10) << "encode_pending v " << (paxos->get_version() + 1) << dendl;
  for (multimap<utime_t,ClassLibraryIncremental>::iterator p = pending_class.begin();
       p != pending_class.end();
       p++)
    p->second.encode(bl);
}

bool ClassMonitor::preprocess_query(Message *m)
{
  dout(10) << "preprocess_query " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return preprocess_command((MMonCommand*)m);

  case MSG_LOG:
    return preprocess_class((MClass*)m);

  default:
    assert(0);
    delete m;
    return true;
  }
}

bool ClassMonitor::prepare_update(Message *m)
{
  dout(10) << "prepare_update " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return prepare_command((MMonCommand*)m);
  case MSG_CLASS:
    return prepare_class((MClass*)m);
  default:
    assert(0);
    delete m;
    return false;
  }
}

void ClassMonitor::committed()
{

}

bool ClassMonitor::preprocess_class(MClass *m)
{
  dout(10) << "preprocess_class " << *m << " from " << m->get_orig_source() << dendl;
  
  int num_new = 0;
  for (deque<ClassLibrary>::iterator p = m->info.begin();
       p != m->info.end();
       p++) {
    if (!pending_list.contains((*p).name))
      num_new++;
  }
  if (!num_new) {
    dout(10) << "  nothing new" << dendl;
    return true;
  }
  return false;
}

bool ClassMonitor::prepare_class(MClass *m) 
{
  dout(10) << "prepare_class " << *m << " from " << m->get_orig_source() << dendl;

  if (ceph_fsid_compare(&m->fsid, &mon->monmap->fsid)) {
    dout(0) << "handle_class on fsid " << m->fsid << " != " << mon->monmap->fsid << dendl;
    delete m;
    return false;
  }
  deque<ClassImpl>::iterator impl_iter = m->impl.begin();

  for (deque<ClassLibrary>::iterator p = m->info.begin();
       p != m->info.end();
       p++, impl_iter++) {
    dout(10) << " writing class " << *p << dendl;
    if (!pending_list.contains((*p).name)) {
      ClassLibraryIncremental inc;
      ::encode(*p, inc.info);
      ::encode(*impl_iter, inc.impl);
      pending_list.add(*p);
      pending_class.insert(pair<utime_t,ClassLibraryIncremental>((*impl_iter).stamp, inc));
    }
  }

  paxos->wait_for_commit(new C_Class(this, m, m->get_orig_source_inst()));
  return true;
}

void ClassMonitor::_updated_class(MClass *m, entity_inst_t who)
{
  dout(7) << "_updated_class for " << who << dendl;
  ClassImpl impl = *(m->impl.rbegin());
  mon->messenger->send_message(new MClassAck(m->fsid, impl.seq), who);
  delete m;
}



bool ClassMonitor::preprocess_command(MMonCommand *m)
{
  int r = -1;
  bufferlist rdata;
  stringstream ss;

  if (r != -1) {
    string rs;
    getline(ss, rs);
    mon->reply_command(m, r, rs, rdata);
    return true;
  } else
    return false;
}


bool ClassMonitor::prepare_command(MMonCommand *m)
{
  stringstream ss;
  string rs;
  int err = -EINVAL;

  // nothing here yet
  ss << "unrecognized command";

  getline(ss, rs);
  mon->reply_command(m, err, rs);
  return false;
}

void ClassMonitor::handle_request(MClass *m)
{
  dout(10) << "handle_request " << *m << " from " << m->get_orig_source() << dendl;
  MClass *reply = new MClass();

  if (!reply)
    return;
  
  for (deque<ClassLibrary>::iterator p = m->info.begin();
       p != m->info.end();
       p++) {
    ClassImpl impl;
    version_t ver;

    reply->info.push_back(*p);

    if (list.get_ver((*p).name, &ver)) {
      char *store_name;
      int len = (*p).name.length() + 16;
      int bin_len;
      store_name = (char *)malloc(len);
      snprintf(store_name, len, "%s.%d", (*p).name.c_str(), ver);
      bin_len = mon->store->get_bl_ss(impl.binary, "class_impl", store_name);
      assert(bin_len > 0);
      dout(0) << "replying with name=" << (*p).name << " version=" << ver <<  " store_name=" << store_name << dendl;
      free(store_name);
      list.add((*p).name, ver);
      reply->add.push_back(true);
      reply->impl.push_back(impl);
    } else {
      reply->add.push_back(false);
    }
  }
  reply->action = CLASS_RESPONSE;
  mon->messenger->send_message(reply, m->get_orig_source_inst());
  delete m;
}

