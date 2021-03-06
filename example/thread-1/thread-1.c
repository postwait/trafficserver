/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/* thread-1.c:  an example program that creates a thread
 *
 *
 *
 *	Usage:
 *	(NT): Thread.dll
 *	(Solaris): thread-1.so
 *
 *
 */

#include <stdio.h>
#include <string.h>
#include <ts/ts.h>

static void *
reenable_txn(void *data)
{
  TSHttpTxn txnp = (TSHttpTxn) data;
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return NULL;
}

static int
thread_plugin(TSCont contp, TSEvent event, void *edata)
{
  switch (event) {
  case TS_EVENT_HTTP_OS_DNS:
      /**
       * Check if the thread has been created successfully or not.
       * If the thread has not been created successfully, assert.
       */
    if (!TSThreadCreate(reenable_txn, edata)) {
      TSReleaseAssert(!"Failure in thread creation");
    }
    return 0;
  default:
    break;
  }
  return 0;
}

int
check_ts_version()
{

  const char *ts_version = TSTrafficServerVersionGet();
  int result = 0;

  if (ts_version) {
    int major_ts_version = 0;
    int minor_ts_version = 0;
    int patch_ts_version = 0;

    if (sscanf(ts_version, "%d.%d.%d", &major_ts_version, &minor_ts_version, &patch_ts_version) != 3) {
      return 0;
    }

    /* Need at least TS 2.0 */
    if (major_ts_version >= 2) {
      result = 1;
    }

  }

  return result;
}

void
TSPluginInit(int argc, const char *argv[])
{
  TSPluginRegistrationInfo info;

  info.plugin_name = "thread-1";
  info.vendor_name = "MyCompany";
  info.support_email = "ts-api-support@MyCompany.com";

  if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
    TSError("Plugin registration failed.\n");
  }

  if (!check_ts_version()) {
    TSError("Plugin requires Traffic Server 3.0 or later\n");
    return;
  }

  TSHttpHookAdd(TS_HTTP_OS_DNS_HOOK, TSContCreate(thread_plugin, NULL));
}
