// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
* Ceph - scalable distributed file system
*
* Copyright (C) 2011 New Dream Network
*
* This is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License version 2.1, as published by the Free Software
* Foundation.  See file COPYING.
*
*/

#include "cross_process_sem.h"
#include "include/rados/librados.h"
#include "st_rados_create_pool.h"
#include "systest_runnable.h"
#include "systest_settings.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sstream>
#include <string>

using std::ostringstream;

static const int RLP_OBJECT_SZ_MAX = 256;

static std::string get_random_buf(void)
{
  ostringstream oss;
  int size = rand() % RLP_OBJECT_SZ_MAX; // yep, it's not very random
  for (int i = 0; i < size; ++i) {
    oss << ".";
  }
  return oss.str();
}

StRadosCreatePool::
StRadosCreatePool(int argc, const char **argv,
		  CrossProcessSem *pool_setup_sem, CrossProcessSem *close_create_pool,
		  int num_objects)
  : SysTestRunnable(argc, argv),
    m_pool_setup_sem(pool_setup_sem), m_close_create_pool(close_create_pool),
    m_num_objects(num_objects)
{
}

StRadosCreatePool::
~StRadosCreatePool()
{
}

int StRadosCreatePool::
run()
{
  rados_t cl;
  RETURN_IF_NONZERO(rados_create(&cl, NULL));
  rados_conf_parse_argv(cl, m_argc, m_argv);
  rados_conf_parse_argv(cl, m_argc, m_argv);
  std::string log_name = SysTestSettings::inst().get_log_name(get_id_str());
  if (!log_name.empty())
    rados_conf_set(cl, "log_file", log_name.c_str());
  RETURN_IF_NONZERO(rados_conf_read_file(cl, NULL));
  RETURN_IF_NONZERO(rados_connect(cl));
  int ret = rados_pool_delete(cl, "foo");
  if (!((ret == 0) || (ret == -ENOENT))) {
    printf("%s: rados_pool_delete error %d\n", get_id_str(), ret);
    return ret;
  }
  RETURN_IF_NONZERO(rados_pool_create(cl, "foo"));
  rados_ioctx_t io_ctx;
  RETURN_IF_NONZERO(rados_ioctx_create(cl, "foo", &io_ctx));

  for (int i = 0; i < m_num_objects; ++i) {
    char oid[128];
    snprintf(oid, sizeof(oid), "%d.obj", i);
    std::string buf(get_random_buf());
    ret = rados_write(io_ctx, oid, buf.c_str(), buf.size(), 0);
    if (ret < static_cast<int>(buf.size())) {
      printf("%s: rados_write error %d\n", get_id_str(), ret);
      return ret;
    }
    if (((i % 25) == 0) || (i == m_num_objects - 1)) {
      printf("%s: created object %d...\n", get_id_str(), i);
    }
  }
  printf("%s: finishing.\n", get_id_str());
  if (m_pool_setup_sem)
    m_pool_setup_sem->post();
  if (m_close_create_pool)
    m_close_create_pool->wait();
  rados_ioctx_destroy(io_ctx);
  rados_shutdown(cl);
  return 0;
}
