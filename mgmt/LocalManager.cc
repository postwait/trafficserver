/** @file

  The Local Manager process of the management system.

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


#include "libts.h"
#include "ink_platform.h"
#include "ink_unused.h"       /* MAGIC_EDITING_TAG */
#include "MgmtUtils.h"
#include "I_Layout.h"
#include "Compatability.h"
#include "LocalManager.h"
#include "MgmtSocket.h"

#if TS_USE_POSIX_CAP
#include <sys/capability.h>
#endif

int bindProxyPort(int, char*, int, bool, int);

bool
LocalManager::SetForDup(void *hIOCPort, long lTProcId, void *hTh)
{
  NOWARN_UNUSED(hIOCPort);
  NOWARN_UNUSED(lTProcId);
  NOWARN_UNUSED(hTh);
  return true;
}

void
LocalManager::mgmtCleanup()
{
  close_socket(process_server_sockfd);

  // fix me for librecords

  if (virt_map) {
    virt_map->rl_downAddrs();   // We are bailing done need to worry about table
  }
#ifdef MGMT_USE_SYSLOG
  closelog();
#endif /* MGMT_USE_SYSLOG */
  return;
}


void
LocalManager::mgmtShutdown(int status, bool mainThread)
{
  if (mainThread) {
    mgmt_log("[LocalManager::mgmtShutdown] Executing shutdown request.\n");
    processShutdown(mainThread);
    // WCCP TBD: Send a shutdown message to routers.

    if (processRunning()) {
      waitpid(watched_process_pid, &status, 0);
#if defined(linux)
      /* Avert race condition, wait for the thread to complete,
         before getting one more restart process */
      /* Workaround for bugid INKqa10060 */
      mgmt_sleep_msec(1);
#endif
    }

    mgmtCleanup();
    _exit(status);
  } else {
    mgmt_shutdown_outstanding = true;
  }
  return;
}


void
LocalManager::processShutdown(bool mainThread)
{
  mgmt_log("[LocalManager::processShutdown] Executing process shutdown request.\n");
  if (mainThread) {
    sendMgmtMsgToProcesses(MGMT_EVENT_SHUTDOWN, "processShutdown[main]");
  } else {
    signalEvent(MGMT_EVENT_SHUTDOWN, "processShutdown");
  }
  return;
}


void
LocalManager::processRestart()
{
  mgmt_log("[LocalManager::processRestart] Executing process restart request.\n");
  signalEvent(MGMT_EVENT_RESTART, "processRestart");
  return;
}


void
LocalManager::processBounce()
{
  mgmt_log("[LocalManager::processBounce] Executing process bounce request.\n");
  signalEvent(MGMT_EVENT_BOUNCE, "processBounce");
  return;
}

void
LocalManager::rollLogFiles()
{
  mgmt_log("[LocalManager::rollLogFiles] Log files are being rolled.\n");
  signalEvent(MGMT_EVENT_ROLL_LOG_FILES, "rollLogs");
  return;
}

void
LocalManager::clearStats()
{
  char *statsPath;

  // Clear our records and then send the signal.  There is a race condition
  //  here where our stats could get re-updated from the proxy
  //  before the proxy clears them, but this should be rare.
  //
  //  Doing things in the opposite order prevents that race
  //   but excerbates the race between the node and cluster
  //   stats getting cleared by progation of clearing the
  //   cluster stats
  //
  RecResetStatRecord(RECT_NULL, true);

  // If the proxy is not running, sending the signal does
  //   not do anything.  Remove the stats file to make sure
  //   that operation works even when the proxy is off
  //
  if (this->proxy_running == 0) {
    statsPath = Layout::relative_to(Layout::get()->runtimedir, REC_RAW_STATS_FILE);
    if (unlink(statsPath) < 0) {
      if (errno != ENOENT) {
        mgmt_log(stderr, "[LocalManager::clearStats] Unlink of %s failed : %s\n", REC_RAW_STATS_FILE, strerror(errno));
      }
    }
    ats_free(statsPath);
  }
}

// void LocalManager::syslogThrInit()
//
//    On the DEC, syslog is per thread.  This function
//      allows a thread to init syslog with the appropriate
//      configuration
//
void
LocalManager::syslogThrInit()
{
}

// bool LocalManager::clusterOk()
//
//   Returns false if the proxy has been up for more than
//     30 seconds but is not reporting that it has clustered
//     with all the nodes in cluster.config
//
//   Otherwise returns true
//
bool
LocalManager::clusterOk()
{
  bool found = true;
  bool result = true;

  if (processRunning() == true && time(NULL) > (this->proxy_started_at + 30)
      && this->ccom->alive_peers_count + 1 != REC_readInteger("proxy.process.cluster.nodes", &found)) {
    result = false;
  }

  ink_assert(found);
  return result;
}

bool
LocalManager::processRunning()
{
  if (watched_process_fd != -1 && watched_process_pid != -1) {
    return true;
  } else {
    return false;
  }
}

LocalManager::LocalManager(char *mpath, bool proxy_on)
  : BaseManager(), run_proxy(proxy_on)
{
  NOWARN_UNUSED(mpath);
  bool found;
#ifdef MGMT_USE_SYSLOG
  syslog_facility = 0;
#endif

  ccom = NULL;
  proxy_started_at = -1;
  proxy_launch_count = 0;
  manager_started_at = time(NULL);
  proxy_launch_outstanding = false;
  mgmt_shutdown_outstanding = false;
  proxy_running = 0;
  RecSetRecordInt("proxy.node.proxy_running", 0);
  mgmt_sync_key = REC_readInteger("proxy.config.lm.sem_id", &found);
  if (!found || mgmt_sync_key <= 0) {
    mgmt_log("Bad or missing proxy.config.lm.sem_id value; using default id %d\n", MGMT_SEMID_DEFAULT);
    mgmt_sync_key = MGMT_SEMID_DEFAULT;
  }
  ink_strlcpy(pserver_path, system_runtime_dir, sizeof(pserver_path));

  virt_map = NULL;


  for (int i = 0; i < MAX_PROXY_SERVER_PORTS; i++) {
    proxy_server_port[i] = proxy_server_fd[i] = -1;
    memset((void *) proxy_server_port_attributes[i], 0, MAX_ATTR_LEN);
  }

  int pnum = 0;
  RecInt http_enabled = REC_readInteger("proxy.config.http.enabled", &found);
  ink_debug_assert(found);
  if (http_enabled && found) {
    int port = (int) REC_readInteger("proxy.config.http.server_port", &found);
    if (found) {
      proxy_server_port[pnum] = port;
      char *main_server_port_attributes = (char *) REC_readString("proxy.config.http.server_port_attr", &found);
      ink_strlcpy((char *) proxy_server_port_attributes[pnum],
                  main_server_port_attributes, sizeof(proxy_server_port_attributes[pnum]));
      ats_free(main_server_port_attributes);
      pnum++;
    }
  }

  // Check to see if we are running SSL term
  RecInt ssl_term_enabled = REC_readInteger("proxy.config.ssl.enabled", &found);
  ink_assert(found);
  if (found && ssl_term_enabled) {
    // Get the SSL port
    RecInt ssl_term_port = REC_readInteger("proxy.config.ssl.server_port", &found);
    ink_assert(found);
    if (found) {
      proxy_server_port[pnum] = ssl_term_port;
      ink_strlcpy((char *) proxy_server_port_attributes[pnum], "S", sizeof(proxy_server_port_attributes[pnum]));
      pnum++;
    }
  }

  // Read other ports to be listened on
  char *proxy_server_other_ports = REC_readString("proxy.config.http.server_other_ports", &found);
  if (proxy_server_other_ports) {
    char *last, *cport;

    cport = ink_strtok_r(proxy_server_other_ports, " ", &last);
    for (; pnum < MAX_PROXY_SERVER_PORTS && cport; pnum++) {
      const char *attr = "X";

      for (int j = 0; cport[j]; j++) {
        if (cport[j] == ':') {
          cport[j] = '\0';
          attr = &cport[j + 1];
        }
      }

      int port_no = atoi(cport);

      proxy_server_port[pnum] = port_no;
      ink_strlcpy((char *) proxy_server_port_attributes[pnum], attr, sizeof(proxy_server_port_attributes[pnum]));

      Debug("lm", "[LocalManager::LocalManager] Adding port (%s, %d, '%s')\n", cport, port_no, attr);
      cport = ink_strtok_r(NULL, " ", &last);
    }

    if (pnum == MAX_PROXY_SERVER_PORTS && cport) {
      Debug("lm", "[LocalManager::LocalManager] Unable to listen on all other ports,"
            " max number of other ports exceeded(max == %d)\n", MAX_PROXY_SERVER_PORTS);
    }
    ats_free(proxy_server_other_ports);
  }
  // Bind proxy ports to incoming_ip_to_bind
  char *incoming_ip_to_bind_str = REC_readString("proxy.local.incoming_ip_to_bind", &found);
  if (found && incoming_ip_to_bind_str != NULL) {
    proxy_server_incoming_ip_to_bind_str = incoming_ip_to_bind_str;
  } else {
    proxy_server_incoming_ip_to_bind_str = NULL;
  }
  config_path = REC_readString("proxy.config.config_dir", &found);
  char *absolute_config_path = Layout::get()->relative(config_path);
  ats_free(config_path);
  if (access(absolute_config_path, R_OK) == -1) {
    config_path = ats_strdup(system_config_directory);
    if (access(config_path, R_OK) == -1) {
        mgmt_elog("[LocalManager::LocalManager] unable to access() directory '%s': %d, %s\n",
                config_path, errno, strerror(errno));
        mgmt_fatal("[LocalManager::LocalManager] please set config path via command line '-path <path>' or 'proxy.config.config_dir' \n");
    }
  } else {
    config_path = absolute_config_path;
  }

#if TS_HAS_WCCP
  // Bind the WCCP address if present.
  xptr<char> wccp_addr_str(REC_readString("proxy.config.wccp.addr", &found));
  if (found && wccp_addr_str && *wccp_addr_str) {
    wccp_cache.setAddr(inet_addr(wccp_addr_str));
    mgmt_log("[LocalManager::LocalManager] WCCP identifying address set to %s.\n", static_cast<char*>(wccp_addr_str));
  }

  xptr<char> wccp_config_str(REC_readString("proxy.config.wccp.services", &found));
  if (found && wccp_config_str && *wccp_config_str) {
    bool located = true;
    if (access(wccp_config_str, R_OK) == -1) {
      wccp_config_str = Layout::relative_to(Layout::get()->sysconfdir, wccp_config_str);
      if (access(wccp_config_str, R_OK) == -1 ) {
        located = false;
      }
    }
    if (located) {
      wccp_cache.loadServicesFromFile(wccp_config_str);
    } else { // not located
      mgmt_log("[LocalManager::LocalManager] WCCP service configuration file '%s' was specified but could not be found in the file system.\n", static_cast<char*>(wccp_config_str));
    }
  }
#endif

  bin_path = REC_readString("proxy.config.bin_path", &found);
  process_server_timeout_secs = REC_readInteger("proxy.config.lm.pserver_timeout_secs", &found);
  process_server_timeout_msecs = REC_readInteger("proxy.config.lm.pserver_timeout_msecs", &found);
  proxy_name = REC_readString("proxy.config.proxy_name", &found);
  proxy_binary = REC_readString("proxy.config.proxy_binary", &found);
  proxy_options = REC_readString("proxy.config.proxy_binary_opts", &found);
  env_prep = REC_readString("proxy.config.env_prep", &found);
  // Calculate configured bin_path from the prefix
  char *absolute_bin_path = Layout::get()->relative(bin_path);
  ats_free(bin_path);
  bin_path = absolute_bin_path;
  // Calculate proxy_binary from the absolute bin_path
  absolute_proxy_binary = Layout::relative_to(absolute_bin_path, proxy_binary);

  if (access(absolute_proxy_binary, R_OK | X_OK) == -1) {
    // Try 'Layout::bindir' directory
    ats_free(absolute_proxy_binary);
    absolute_proxy_binary = Layout::relative_to(Layout::get()->bindir, proxy_binary);
    // coverity[fs_check_call]
    if (access(absolute_proxy_binary, R_OK | X_OK) == -1) {
        mgmt_elog("[LocalManager::LocalManager] Unable to access() '%s': %d, %s\n",
                absolute_proxy_binary, errno, strerror(errno));
        mgmt_fatal("[LocalManager::LocalManager] please set bin path 'proxy.config.bin_path' \n");
    }
  }

  internal_ticker = 0;

  watched_process_pid = -1;

  process_server_sockfd = -1;
  watched_process_fd = -1;
  proxy_launch_pid = -1;

  return;
}

void
LocalManager::initAlarm()
{
  alarm_keeper = new Alarms();
}

/*
 * initCCom(...)
 *   Function initializes cluster communication structure held by local manager.
 */
void
LocalManager::initCCom(int port, char *addr, int sport)
{
  bool found;
  ts_ip_endpoint cluster_ip;    // ip addr of the cluster interface
  ip_text_buffer clusterAddrStr;         // cluster ip addr as a String
  char *intrName;               // Name of the interface we are to use
  char hostname[1024];          // hostname of this machine
  const char envVar[] = "PROXY_CLUSTER_ADDR=";
  char *envBuf;

  if (gethostname(hostname, 1024) < 0) {
    mgmt_fatal(stderr, "[LocalManager::initCCom] gethostname failed\n");
  }
  // Fetch which interface we are using for clustering
  intrName = REC_readString("proxy.config.cluster.ethernet_interface", &found);
  ink_assert(intrName != NULL);

  found = mgmt_getAddrForIntr(intrName, &cluster_ip.sa);
  if (found == false) {
    mgmt_fatal(stderr, "[LocalManager::initCCom] Unable to find network interface %s.  Exiting...\n", intrName);
  } else if (!ink_inet_is_ip4(&cluster_ip)) {
    mgmt_fatal(stderr, "[LocalManager::initCCom] Unable to find IPv4 network interface %s.  Exiting...\n", intrName);
  }

  ink_inet_ntop(&cluster_ip, clusterAddrStr, sizeof(clusterAddrStr));
  Debug("ccom", "Cluster Interconnect is %s : %s\n", intrName, clusterAddrStr);

  // This an awful hack but I could not come up with a better way to
  //  pass the cluster address to the proxy
  //    Set an environment variable so the proxy can find out
  //    what the cluster address is.  The reason we need this awful
  //    hack is that the proxy needs this info immediately at startup
  //    and it is different for every machine in the cluster so using
  //    a config variable will not work.
  // The other options are to pass it on the command line to the proxy
  //    which would require a fair bit of code modification since
  //    what is passed right now is assumed to be static.  The other
  //    is to write it to a separate file but that seems like a
  //    lot of trouble for a 16 character string
  // Set the cluster ip addr variable so that proxy can read it
  //    and flush it to disk
  const size_t envBuf_size = strlen(envVar) + strlen(clusterAddrStr) + 1;
  envBuf = (char *)ats_malloc(envBuf_size);
  ink_strlcpy(envBuf, envVar, envBuf_size);
  ink_strlcat(envBuf, clusterAddrStr, envBuf_size);
  ink_release_assert(putenv(envBuf) == 0);

  ccom = new ClusterCom(ink_inet_ip4_addr_cast(&cluster_ip), hostname, port, addr, sport, pserver_path);
  virt_map = new VMap(intrName, ink_inet_ip4_addr_cast(&cluster_ip), &lmgmt->ccom->mutex);
  virt_map->downAddrs();        // Just to be safe
  ccom->establishChannels();
  ats_free(intrName);

  return;
}

/*
 * initMgmtProcessServer()
 * - On UNIX, this function sets up the server socket that proxy processes connect to.
 * - On WIN32, named pipes are used instead.
 */
void
LocalManager::initMgmtProcessServer()
{
  char fpath[1024];
  int servlen, one = 1;
  struct sockaddr_un serv_addr;

#if TS_HAS_WCCP
  if (wccp_cache.isConfigured()) {
    if (0 > wccp_cache.open()) mgmt_log("Failed to open WCCP socket\n");
  }
#endif

  snprintf(fpath, sizeof(fpath), "%s/%s", pserver_path, LM_CONNECTION_SERVER);
  unlink(fpath);
  if ((process_server_sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    mgmt_fatal(stderr, "[LocalManager::initMgmtProcessServer] Unable to open socket exiting\n");
  }

  if (fcntl(process_server_sockfd, F_SETFD, 1) < 0) {
    mgmt_fatal(stderr, "[LocalManager::initMgmtProcessServer] Unable to set close-on-exec\n");
  }

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sun_family = AF_UNIX;
  ink_strlcpy(serv_addr.sun_path, fpath, sizeof(serv_addr.sun_path));
#if defined(darwin) || defined(freebsd)
  servlen = sizeof(struct sockaddr_un);
#else
  servlen = strlen(serv_addr.sun_path) + sizeof(serv_addr.sun_family);
#endif
  if (setsockopt(process_server_sockfd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(int)) < 0) {
    mgmt_fatal(stderr, "[LocalManager::initMgmtProcessServer] Unable to set socket options.\n");
  }

  if ((bind(process_server_sockfd, (struct sockaddr *) &serv_addr, servlen)) < 0) {
    mgmt_fatal(stderr, "[LocalManager::initMgmtProcessServer] Unable to bind '%s' socket exiting\n", fpath);
  }

  if ((listen(process_server_sockfd, 5)) < 0) {
    mgmt_fatal(stderr, "[LocalManager::initMgmtProcessServer] Unable to listen on socket exiting\n");
  }

  RecSetRecordInt("proxy.node.restarts.manager.start_time", manager_started_at);
}

/*
 * pollMgmtProcessServer()
 * -  Function checks the mgmt process server for new processes
 *    and any requests sent from processes. It handles processes sent.
 */
void
LocalManager::pollMgmtProcessServer()
{
  int num;
  struct timeval timeout;
  struct sockaddr_in clientAddr;
  fd_set fdlist;
#if TS_HAS_WCCP
  int wccp_fd = wccp_cache.getSocket();
#endif

  while (1) {
    // poll only
    timeout.tv_sec = process_server_timeout_secs;
    timeout.tv_usec = process_server_timeout_msecs * 1000;
    FD_ZERO(&fdlist);
    FD_SET(process_server_sockfd, &fdlist);
    if (watched_process_fd != -1) FD_SET(watched_process_fd, &fdlist);

#if TS_HAS_WCCP
    // Only run WCCP housekeeping while we have a server process.
    // Note: The WCCP socket is opened iff WCCP is configured.
    if (wccp_fd != ts::NO_FD && watched_process_fd != ts::NO_FD) {
      wccp_cache.housekeeping();
      time_t wccp_wait = wccp_cache.waitTime();
      if (wccp_wait < process_server_timeout_secs) timeout.tv_sec = wccp_wait;
      FD_SET(wccp_cache.getSocket(), &fdlist);
    }
#endif

    num = mgmt_select(FD_SETSIZE, &fdlist, NULL, NULL, &timeout);
    if (num == 0) {             /* Have nothing */
      break;
    } else if (num > 0) {       /* Have something */
#if TS_HAS_WCCP
      if (wccp_fd != ts::NO_FD && FD_ISSET(wccp_fd, &fdlist)) {
        wccp_cache.handleMessage();
        --num;
      }
#endif
      if (FD_ISSET(process_server_sockfd, &fdlist)) {   /* New connection */
        int clientLen = sizeof(clientAddr);
        int new_sockfd = mgmt_accept(process_server_sockfd,
                                     (struct sockaddr *) &clientAddr,
                                     &clientLen);
        MgmtMessageHdr *mh;
        int data_len;

        mgmt_log(stderr, "[LocalManager::pollMgmtProcessServer] New process connecting fd '%d'\n", new_sockfd);

        if (new_sockfd < 0) {
          mgmt_elog(stderr, "[LocalManager::pollMgmtProcessServer] ==> ");
        } else if (!processRunning()) {
          watched_process_fd = new_sockfd;
          data_len = sizeof(mgmt_sync_key);
          mh = (MgmtMessageHdr *) alloca(sizeof(MgmtMessageHdr) + data_len);
          mh->msg_id = MGMT_EVENT_SYNC_KEY;
          mh->data_len = data_len;
          memcpy((char *) mh + sizeof(MgmtMessageHdr), &mgmt_sync_key, data_len);
          if (mgmt_write_pipe(new_sockfd, (char *) mh, sizeof(MgmtMessageHdr) + data_len) <= 0) {
            mgmt_elog("[LocalManager::pollMgmtProcessServer] Error writing sync key message!\n");
            close_socket(new_sockfd);
            watched_process_fd = watched_process_pid = -1;
          }
        } else {
          close_socket(new_sockfd);
        }
        --num;
      }

      if (ts::NO_FD != watched_process_fd && FD_ISSET(watched_process_fd, &fdlist)) {
        int res;
        MgmtMessageHdr mh_hdr;
        MgmtMessageHdr *mh_full;
        char *data_raw;

        // read the message
        if ((res = mgmt_read_pipe(watched_process_fd, (char *) &mh_hdr, sizeof(MgmtMessageHdr))) > 0) {
          mh_full = (MgmtMessageHdr *) alloca(sizeof(MgmtMessageHdr) + mh_hdr.data_len);
          memcpy(mh_full, &mh_hdr, sizeof(MgmtMessageHdr));
          data_raw = (char *) mh_full + sizeof(MgmtMessageHdr);
          if ((res = mgmt_read_pipe(watched_process_fd, data_raw, mh_hdr.data_len)) > 0) {
            handleMgmtMsgFromProcesses(mh_full);
          } else if (res < 0) {
            mgmt_fatal("[LocalManager::pollMgmtProcessServer] Error in read (errno: %d)\n", -res);
          }
        } else if (res < 0) {
          mgmt_fatal("[LocalManager::pollMgmtProcessServer] Error in read (errno: %d)\n", -res);
        }
        // handle EOF
        if (res == 0) {
          int estatus;
          pid_t tmp_pid = watched_process_pid;

          Debug("lm", "[LocalManager::pollMgmtProcessServer] Lost process EOF!\n");

          close_socket(watched_process_fd);

          waitpid(watched_process_pid, &estatus, 0);    /* Reap child */
          if (WIFSIGNALED(estatus)) {
            int sig = WTERMSIG(estatus);
            mgmt_elog(stderr, "[LocalManager::pollMgmtProcessServer] "
                      "Server Process terminated due to Sig %d: %s\n", sig, strsignal(sig));
          }

          if (lmgmt->run_proxy) {
            mgmt_elog("[Alarms::signalAlarm] Server Process was reset\n");
            lmgmt->alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_PROCESS_DIED);
          } else {
            mgmt_log("[TrafficManager] Server process shutdown\n");
          }

          watched_process_fd = watched_process_pid = -1;
          if (tmp_pid != -1) {  /* Incremented after a pid: message is sent */
            proxy_running--;
          }
          proxy_started_at = -1;
          RecSetRecordInt("proxy.node.proxy_running", 0);
        }

        num--;

      }
      ink_assert(num == 0);     /* Invariant */

    } else if (num < 0) {       /* Error */
      mgmt_elog(stderr, "[LocalManager::pollMgmtProcessServer] select failed or was interrupted (%d)\n", errno);
    }

  }
}


void
LocalManager::handleMgmtMsgFromProcesses(MgmtMessageHdr * mh)
{
  char *data_raw = (char *) mh + sizeof(MgmtMessageHdr);
  switch (mh->msg_id) {
  case MGMT_SIGNAL_PID:
    watched_process_pid = *((pid_t *) data_raw);
    lmgmt->alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_PROCESS_BORN);
    proxy_running++;
    proxy_launch_pid = -1;
    proxy_launch_outstanding = false;
    RecSetRecordInt("proxy.node.proxy_running", 1);
    break;

  case MGMT_SIGNAL_MACHINE_UP:
    /*
       {
       struct in_addr addr;
       addr.s_addr = *((unsigned int*)data_raw);
       alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_PEER_BORN, inet_ntoa(addr));
       }
     */
    break;
  case MGMT_SIGNAL_MACHINE_DOWN:
    /*
       {
       struct in_addr addr;
       addr.s_addr = *((unsigned int*)data_raw);
       alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_PEER_DIED, inet_ntoa(addr));
       }
     */
    break;

    // FIX: This is very messy need to correlate mgmt signals and
    // alarms better
  case MGMT_SIGNAL_CONFIG_ERROR:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_CONFIG_ERROR, data_raw);
    break;
  case MGMT_SIGNAL_SYSTEM_ERROR:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_SYSTEM_ERROR, data_raw);
    break;
  case MGMT_SIGNAL_LOG_SPACE_CRISIS:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_LOG_SPACE_CRISIS, data_raw);
    break;
  case MGMT_SIGNAL_CACHE_ERROR:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_CACHE_ERROR, data_raw);
    break;
  case MGMT_SIGNAL_CACHE_WARNING:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_CACHE_WARNING, data_raw);
    break;
  case MGMT_SIGNAL_LOGGING_ERROR:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_LOGGING_ERROR, data_raw);
    break;
  case MGMT_SIGNAL_LOGGING_WARNING:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_LOGGING_WARNING, data_raw);
    break;
  case MGMT_SIGNAL_CONFIG_FILE_READ:
    mgmt_log(stderr, "[LocalManager::handleMgmtMsgFromProcesses] File done '%d'\n", data_raw);
    break;
  case MGMT_SIGNAL_PLUGIN_ADD_REC:
    {
      char var_name[256];
      char var_value[256];
      RecDataT data_type;
      // data_type is an enum type, so cast to an int* to avoid warnings. /leif
      // coverity[secure_coding]
      if (sscanf(data_raw, "%255s %d %255s", var_name, (int *) &data_type, var_value) != 3) {
        Debug("lm", "Warning: Bad data_type: %s", (char *) data_raw);
        data_type = RECD_MAX;
      }
      switch (data_type) {
      case RECD_COUNTER:
        RecRegisterStatCounter(RECT_PLUGIN, var_name, ink_atoi64(var_value), RECP_NULL);
        break;
      case RECD_INT:
        RecRegisterStatInt(RECT_PLUGIN, var_name, ink_atoi64(var_value), RECP_NULL);
        break;
      case RECD_FLOAT:
        RecRegisterStatFloat(RECT_PLUGIN, var_name, atof(var_value), RECP_NULL);
        break;
      case RECD_STRING:
        RecRegisterStatString(RECT_PLUGIN, var_name, var_value, RECP_NULL);
        break;
      default:
        break;
      }
      break;
    }
  case MGMT_SIGNAL_PLUGIN_SET_CONFIG:
    {
      char var_name[256];
      char var_value[256];
      MgmtType stype;
      // stype is an enum type, so cast to an int* to avoid warnings. /leif
      int tokens = sscanf(data_raw, "%255s %d %255s", var_name, (int *) &stype, var_value);
      if (tokens != 3) {
        stype = MGMT_INVALID;
      }
      switch (stype) {
      case MGMT_INT:
        RecSetRecordInt(var_name, ink_atoi64(var_value));
        break;
      case MGMT_COUNTER:
      case MGMT_FLOAT:
      case MGMT_STRING:
      case MGMT_INVALID:
      default:
        mgmt_elog(stderr,
                  "[LocalManager::handleMgmtMsgFromProcesses] " "Invalid plugin set-config msg '%s'\n", data_raw);
        break;
      }
    }
  case MGMT_SIGNAL_LOG_FILES_ROLLED:
    {
      Debug("lm", "Rolling logs %s", (char *) data_raw);
      break;
    }
  case MGMT_SIGNAL_LIBRECORDS:
    if (mh->data_len > 0) {
      executeMgmtCallback(MGMT_SIGNAL_LIBRECORDS, data_raw, mh->data_len);
    } else {
      executeMgmtCallback(MGMT_SIGNAL_LIBRECORDS, NULL, 0);
    }
    break;
    // Congestion Control - begin
  case MGMT_SIGNAL_HTTP_CONGESTED_SERVER:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_HTTP_CONGESTED_SERVER, data_raw);
    break;
  case MGMT_SIGNAL_HTTP_ALLEVIATED_SERVER:
    alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_HTTP_ALLEVIATED_SERVER, data_raw);
    break;
    // Congestion Control - end
  case INK_MGMT_SIGNAL_WDA_BILLING_CONNECTION_DIED:
    alarm_keeper->signalAlarm(MGMT_ALARM_WDA_BILLING_CONNECTION_DIED, data_raw);
    break;
  case INK_MGMT_SIGNAL_WDA_BILLING_CORRUPTED_DATA:
    alarm_keeper->signalAlarm(MGMT_ALARM_WDA_BILLING_CORRUPTED_DATA, data_raw);
    break;
  case INK_MGMT_SIGNAL_WDA_XF_ENGINE_DOWN:
    alarm_keeper->signalAlarm(MGMT_ALARM_WDA_XF_ENGINE_DOWN, data_raw);
    break;
    // Wireless plugin signal - end
  case INK_MGMT_SIGNAL_SAC_SERVER_DOWN:
    alarm_keeper->signalAlarm(MGMT_ALARM_SAC_SERVER_DOWN, data_raw);
    break;

  default:
    break;
  }

  // #define MGMT_ALARM_ACC_ALARMS_START              200
  // #define MGMT_ALARM_ACC_ALARMS_END                299

  if (mh->msg_id >= INK_MGMT_SIGNAL_ACC_ALARMS_START && mh->msg_id <= INK_MGMT_SIGNAL_ACC_ALARMS_END) {
    alarm_keeper->signalAlarm(mh->msg_id, data_raw);
  }
}


void
LocalManager::sendMgmtMsgToProcesses(int msg_id, const char *data_str)
{
  sendMgmtMsgToProcesses(msg_id, data_str, strlen(data_str) + 1);
  return;
}


void
LocalManager::sendMgmtMsgToProcesses(int msg_id, const char *data_raw, int data_len)
{
  MgmtMessageHdr *mh;

  mh = (MgmtMessageHdr *) alloca(sizeof(MgmtMessageHdr) + data_len);
  mh->msg_id = msg_id;
  mh->data_len = data_len;
  memcpy((char *) mh + sizeof(MgmtMessageHdr), data_raw, data_len);
  sendMgmtMsgToProcesses(mh);
  return;
}


void
LocalManager::sendMgmtMsgToProcesses(MgmtMessageHdr * mh)
{
  switch (mh->msg_id) {
  case MGMT_EVENT_SHUTDOWN:{
      run_proxy = false;
      if (lmgmt->virt_map) {
        lmgmt->virt_map->downAddrs();   /* Down all known addrs to be safe */
      }
      for (int i = 0; i < MAX_PROXY_SERVER_PORTS; i++) {
        if (proxy_server_fd[i] != -1) { // Close the socket
          close_socket(proxy_server_fd[i]);
          proxy_server_fd[i] = -1;
        }
      }
      break;
    }
  case MGMT_EVENT_RESTART:
    run_proxy = true;
    listenForProxy();
    return;
  case MGMT_EVENT_BOUNCE:      /* Just bouncing the cluster, have it exit well restart */
    mh->msg_id = MGMT_EVENT_SHUTDOWN;
    break;
  case MGMT_EVENT_ROLL_LOG_FILES:
    mgmt_log("[LocalManager::SendMgmtMsgsToProcesses]Event is being constructed .\n");
    break;
  case MGMT_EVENT_CONFIG_FILE_UPDATE:
    bool found;
    char *fname;
    Rollback *rb;
    char *data_raw;

    data_raw = (char *) mh + sizeof(MgmtMessageHdr);
    fname = REC_readString(data_raw, &found);

    RecT rec_type;
    if (RecGetRecordType(data_raw, &rec_type) == REC_ERR_OKAY && rec_type == RECT_CONFIG) {
      RecSetSyncRequired(data_raw);
    } else {
      mgmt_elog(stderr, "[LocalManager:sendMgmtMsgToProcesses] Unknown file change: '%s'\n", data_raw);
    }
    ink_assert(found);
    if (!(configFiles->getRollbackObj(fname, &rb)) &&
        (strcmp(data_raw, "proxy.config.cluster.cluster_configuration") != 0) &&
        (strcmp(data_raw, "proxy.config.arm.acl_filename_master") != 0) &&
        (strcmp(data_raw, "proxy.config.body_factory.template_sets_dir") != 0)) {
      mgmt_elog(stderr, "[LocalManager::sendMgmtMsgToProcesses] "
                "Invalid 'data_raw' for MGMT_EVENT_CONFIG_FILE_UPDATE\n");
      ink_assert(false);
    }
    ats_free(fname);
    break;
  }

  if (watched_process_fd != -1) {
    if (mgmt_write_pipe(watched_process_fd, (char *) mh, sizeof(MgmtMessageHdr) + mh->data_len) <= 0) {
      // In case of Linux, sometimes when the TS dies, the connection between TS and TM
      // is not closed properly. the socket does not receive an EOF. So, the TM does
      // not detect that the connection and hence TS has gone down. Hence it still
      // tries to send a message to TS, but encounters an error and enters here
      // Also, ensure that this whole thing is done only once because there will be a
      // deluge of message in the traffic.log otherwise

      static pid_t check_prev_pid = watched_process_pid;
      static pid_t check_current_pid = watched_process_pid;
      if (check_prev_pid != watched_process_pid) {
        check_prev_pid = watched_process_pid;
        check_current_pid = watched_process_pid;
      }

      if (check_prev_pid == check_current_pid) {
        check_current_pid = -1;
        int lerrno = errno;
        mgmt_elog(stderr, "[LocalManager::sendMgmtMsgToProcesses] Error writing message\n");
        if (lerrno == ECONNRESET || lerrno == EPIPE) {  // Connection closed by peer or Broken pipe
          if ((kill(watched_process_pid, 0) < 0) && (errno == ESRCH)) {
            // TS is down
            pid_t tmp_pid = watched_process_pid;
            close_socket(watched_process_fd);
            mgmt_elog(stderr, "[LocalManager::pollMgmtProcessServer] " "Server Process has been terminated\n");
            if (lmgmt->run_proxy) {
              mgmt_elog("[Alarms::signalAlarm] Server Process was reset\n");
              lmgmt->alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_PROCESS_DIED);
            } else {
              mgmt_log("[TrafficManager] Server process shutdown\n");
            }
            watched_process_fd = watched_process_pid = -1;
            if (tmp_pid != -1) {        /* Incremented after a pid: message is sent */
              proxy_running--;
            }
            proxy_started_at = -1;
            RecSetRecordInt("proxy.node.proxy_running", 0);
            // End of TS down
          } else {
            // TS is still up, but the connection is lost
            const char *err_msg =
              "The TS-TM connection is broken for some reason. Either restart TS and TM or correct this error for TM to display TS statistics correctly";
            lmgmt->alarm_keeper->signalAlarm(MGMT_ALARM_PROXY_SYSTEM_ERROR, err_msg);
          }

          // check if the TS is down, by checking the pid
          // if TS is down, then,
          //     raise an alarm
          //     set the variables so that TS is restarted later
          // else (TS is still up)
          //     raise an alarm stating the problem
        }
      }
    }
  }
}


void
LocalManager::signalFileChange(const char *var_name)
{
  signalEvent(MGMT_EVENT_CONFIG_FILE_UPDATE, var_name);
  return;
}


void
LocalManager::signalEvent(int msg_id, const char *data_str)
{
  signalEvent(msg_id, data_str, strlen(data_str) + 1);
  return;
}


void
LocalManager::signalEvent(int msg_id, const char *data_raw, int data_len)
{
  MgmtMessageHdr *mh;

  mh = (MgmtMessageHdr *)ats_malloc(sizeof(MgmtMessageHdr) + data_len);
  mh->msg_id = msg_id;
  mh->data_len = data_len;
  memcpy((char *) mh + sizeof(MgmtMessageHdr), data_raw, data_len);
  ink_assert(enqueue(mgmt_event_queue, mh));

  return;
}


/*
 * processEventQueue()
 *   Function drains and processes the mgmt event queue
 * notifying any registered callback functions and performing
 * any mgmt tasks for each event.
 */
void
LocalManager::processEventQueue()
{
  bool handled_by_mgmt;

  while (!queue_is_empty(mgmt_event_queue)) {
    handled_by_mgmt = false;

    MgmtMessageHdr *mh = (MgmtMessageHdr *) dequeue(mgmt_event_queue);
    char *data_raw = (char *) mh + sizeof(MgmtMessageHdr);

    // check if we have a local file update
    if (mh->msg_id == MGMT_EVENT_CONFIG_FILE_UPDATE) {
      // records.config
      if (!(strcmp(data_raw, "records.config"))) {
        if (RecReadConfigFile() != REC_ERR_OKAY) {
          mgmt_elog(stderr, "[fileUpdated] Config update failed for records.config\n");
        }
        handled_by_mgmt = true;
      }
    }

    if (!handled_by_mgmt) {
      if (processRunning() == false) {
        // Fix INKqa04984
        // If traffic server hasn't completely come up yet,
        // we will hold off until next round.
        ink_assert(enqueue(mgmt_event_queue, mh));
        return;
      }
      Debug("lm", "[TrafficManager] ==> Sending signal event '%d'\n", mh->msg_id);
      lmgmt->sendMgmtMsgToProcesses(mh);
    }
    ats_free(mh);
  }
}


/*
 * startProxy()
 *   Function fires up a proxy process.
 */
bool
LocalManager::startProxy()
{
  if (proxy_launch_outstanding) {
    return false;
  }
  mgmt_log(stderr, "[LocalManager::startProxy] Launching ts process\n");

  pid_t pid;

  // Before we do anything lets check for the existence of
  // the traffic server binary along with it's execute permmissions
  if (access(absolute_proxy_binary, F_OK) < 0) {
    // Error can't find traffic_server
    mgmt_elog(stderr, "[LocalManager::startProxy] Unable to find traffic server at %s\n", absolute_proxy_binary);
    return false;
  }
  // traffic server binary exists, check permissions
  else if (access(absolute_proxy_binary, R_OK | X_OK) < 0) {
    // Error don't have proper permissions
    mgmt_elog(stderr, "[LocalManager::startProxy] Unable to access %s due to bad permisssions \n",
              absolute_proxy_binary);
    return false;
  }

  if (env_prep) {
#ifdef POSIX_THREAD
    if ((pid = fork()) < 0)
#else
    if ((pid = fork1()) < 0)
#endif
    {
      mgmt_elog(stderr, "[LocalManager::startProxy] Unable to fork1 prep process\n");
      return false;
    } else if (pid > 0) {
      int estatus;
      waitpid(pid, &estatus, 0);
    } else {
      int res;
      char env_prep_bin[1024];

      snprintf(env_prep_bin, sizeof(env_prep_bin), "%s/%s", bin_path, env_prep);
      res = execl(env_prep_bin, env_prep, (char*)NULL);
      _exit(res);
    }
  }
#ifdef POSIX_THREAD
  if ((pid = fork()) < 0)
#else
  if ((pid = fork1()) < 0)
#endif
  {
    mgmt_elog(stderr, "[LocalManager::startProxy] Unable to fork1 process\n");
    return false;
  } else if (pid > 0) {         /* Parent */
    proxy_launch_pid = pid;
    proxy_launch_outstanding = true;
    proxy_started_at = time(NULL);
    ++proxy_launch_count;
    RecSetRecordInt("proxy.node.restarts.proxy.start_time", proxy_started_at);
    RecSetRecordInt("proxy.node.restarts.proxy.restart_count", proxy_launch_count);
  } else {
    int res, i = 0, n;
    char real_proxy_options[2048], *options[32], *last, *tok;

    snprintf(real_proxy_options, sizeof(real_proxy_options), "%s", proxy_options);
    n = strlen(real_proxy_options);

    // Check if we need to pass down port/fd information to
    // traffic_server
    if (proxy_server_fd[0] != -1) {
      snprintf(&real_proxy_options[n], sizeof(real_proxy_options) - n, " -A");
      n = strlen(real_proxy_options);
      // Handle some syntax issues
      if (proxy_server_fd[0] != -1) {
        snprintf(&real_proxy_options[n], sizeof(real_proxy_options) - n, ",");
        n = strlen(real_proxy_options);
      }
      // Fill in the rest of the fd's
      if (proxy_server_fd[0] != -1) {
        snprintf(&real_proxy_options[n], sizeof(real_proxy_options) - n,
                 "%d:%s", proxy_server_fd[0], (char*)proxy_server_port_attributes[0]);
        n = strlen(real_proxy_options);
        for (i = 1; i<MAX_PROXY_SERVER_PORTS && proxy_server_fd[i]> 0; i++) {
          snprintf(&real_proxy_options[n], sizeof(real_proxy_options) - n,
                   ",%d:%s", proxy_server_fd[i], (char*)proxy_server_port_attributes[i]);
          n = strlen(real_proxy_options);
        }
      }
    }

    Debug("lm", "[LocalManager::startProxy] Launching %s with options '%s'\n",
          absolute_proxy_binary, real_proxy_options);

    for (i = 0; i < 32; i++)
      options[i] = NULL;
    options[0] = absolute_proxy_binary;
    i = 1;
    tok = ink_strtok_r(real_proxy_options, " ", &last);
    options[i++] = tok;
    while (i < 32 && (tok = ink_strtok_r(NULL, " ", &last))) {
      options[i++] = tok;
    }

    if (!strstr(proxy_options, "-M")) { // Make sure we're starting the proxy in mgmt mode
      mgmt_fatal(stderr, "[LocalManager::startProxy] ts options must contain -M");
    }

    res = execv(absolute_proxy_binary, options);
    mgmt_elog(stderr, "[LocalManager::startProxy] Exec of %s failed\n", absolute_proxy_binary);
    _exit(res);
  }
  return true;
}


/*
 * listenForProxy()
 *  Function listens on the accept port of the proxy, so users aren't dropped.
 */
void
LocalManager::listenForProxy()
{
  if (!run_proxy)
    return;

  // We are not already bound, bind the port
  for (int i = 0; i < MAX_PROXY_SERVER_PORTS; i++) {
    if (proxy_server_port[i] != -1) {
      if (proxy_server_fd[i] < 0) {
        int domain = AF_INET;
        int type = SOCK_STREAM;
        bool transparent = false;

        switch (*proxy_server_port_attributes[i]) {
        case 'D':
          // D is for DNS proxy, udp only
          type = SOCK_DGRAM;
          break;
        case '>': // in-bound (client side) transparent
        case '=': // fully transparent
          transparent = true;
          break;
        default:
          type = SOCK_STREAM;
        }

        switch (*(proxy_server_port_attributes[i] + 1)) {
        case '6':
          domain = AF_INET6;
          break;
        default:
          domain = AF_INET;
        }

        proxy_server_fd[i] = bindProxyPort(proxy_server_port[i], proxy_server_incoming_ip_to_bind_str, domain, transparent, type);
      }

      if (*proxy_server_port_attributes[i] != 'D') {
        if ((listen(proxy_server_fd[i], 1024)) < 0) {
          mgmt_fatal(stderr, "[LocalManager::listenForProxy] Unable to listen on socket: %d\n", proxy_server_port[i]);
        }
        mgmt_log(stderr, "[LocalManager::listenForProxy] Listening on port: %d\n", proxy_server_port[i]);
      } else {
        break;
      }
    }
  }
  return;
}

#if TS_USE_POSIX_CAP
/** Control file access privileges to bypass DAC.
    @parm state Use @c true to enable elevated privileges,
    @c false to disable.
    @return @c true if successful, @c false otherwise.

    @internal After some pondering I decided that the file access
    privilege was worth the effort of restricting. Unlike the network
    privileges this can protect a host system from programming errors
    by not (usually) permitting such errors to access arbitrary
    files. This is particularly true since none of the config files
    current enable this feature so it's not actually called. Still,
    best to program defensively and have it available.
 */
bool
elevateFileAccess(bool state)
{
  bool zret = false; // return value.
  cap_t cap_state = cap_get_proc(); // current capabilities
  // Make a list of the capabilities we changed.
  cap_value_t cap_list[] = { CAP_DAC_OVERRIDE };
  static int const CAP_COUNT = sizeof(cap_list)/sizeof(*cap_list);

  cap_set_flag(cap_state, CAP_EFFECTIVE, CAP_COUNT, cap_list, state ? CAP_SET : CAP_CLEAR);
  zret = (0 == cap_set_proc(cap_state));
  cap_free(cap_state);
  return zret;
}
#else
//  bool removeRootPriv()
//
//    - Returns true on success
//      and false on failure
//    - no-op on WIN32
bool
removeRootPriv(uid_t euid)
{
  if (seteuid(euid) < 0) {
    Debug("lm", "[removeRootPriv] seteuid failed : %s\n", strerror(errno));
    return false;
  }

  Debug("lm", "[removeRootPriv] removed root privileges.  Euid is %d\n", euid);
  return true;
}

//  bool restoreRootPriv()
//
//    - Returns true on success
//      and false on failure
//    - no-op on WIN32
bool
restoreRootPriv(uid_t *old_euid)
{
  if (old_euid)
    *old_euid = geteuid();
  if (seteuid(0) < 0) {
    Debug("lm", "[restoreRootPriv] seteuid root failed : %s\n", strerror(errno));
    return false;
  }

  Debug("lm", "[restoreRootPriv] restored root privileges.  Euid is %d\n", 0);

  return true;
}
#endif

/*
 * bindProxyPort()
 *  Function binds the accept port of the proxy
 *  Also, type specifies udp or tcp
 */
int
bindProxyPort(int proxy_port, char *incoming_ip_to_bind_str, int domain, bool transparent, int type)
{
  int one = 1;
  int proxy_port_fd = -1;
  struct addrinfo hints;
  struct addrinfo *result = NULL;
  char proxy_port_str[8] = {'\0'};
  int err = 0;

#if !TS_USE_POSIX_CAP
  bool privBoost = false;
  uid_t euid = geteuid();
  uid_t saved_euid = 0;

  if (proxy_port < 1024 && euid != 0) {
    if (restoreRootPriv(&saved_euid) == false) {
      mgmt_elog(stderr, "[bindProxyPort] Unable to get root priviledges to bind port %d. euid is %d.  Exiting\n",
                proxy_port, euid);
      _exit(0);
    } else {
      privBoost = true;
    }
  }
#endif

  /* Setup reliable connection, for large config changes */
  if ((proxy_port_fd = socket(domain, type, 0)) < 0) {
    mgmt_elog(stderr, "[bindProxyPort] Unable to create socket : %s\n", strerror(errno));
    _exit(1);
  }
  if (domain == AF_INET6) {
    if (setsockopt(proxy_port_fd, IPPROTO_IPV6, IPV6_V6ONLY, SOCKOPT_ON, sizeof(int)) < 0) {
      mgmt_elog(stderr, "[bindProxyPort] Unable to set socket options: %d : %s\n", proxy_port, strerror(errno));
    }
  }
  if (setsockopt(proxy_port_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(int)) < 0) {
    mgmt_elog(stderr, "[bindProxyPort] Unable to set socket options: %d : %s\n", proxy_port, strerror(errno));
    _exit(1);
  }

  if (transparent) {
#if TS_USE_TPROXY
    int transparent_value = 1;
    Debug("http_tproxy", "Listen port %d inbound transparency enabled.\n", proxy_port);
    if (setsockopt(proxy_port_fd, SOL_IP, TS_IP_TRANSPARENT, &transparent_value, sizeof(transparent_value)) == -1) {
      mgmt_elog(stderr, "[bindProxyPort] Unable to set transparent socket option [%d] %s\n", errno, strerror(errno));
      _exit(1);
    }
#else
    Debug("lm", "[bindProxyPort] Transparency requested but TPROXY not configured\n");
#endif
  }

  snprintf(proxy_port_str, sizeof(proxy_port_str), "%d", proxy_port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = domain;
  hints.ai_socktype = type;
  hints.ai_flags = AI_PASSIVE;

  err = getaddrinfo(incoming_ip_to_bind_str, proxy_port_str, &hints, &result);
  if (err != 0) {
    mgmt_elog(stderr, "[bindProxyPort] Unable to get address info: %s : %s\n", incoming_ip_to_bind_str, gai_strerror(err));
    _exit(1);
  }

  if((bind(proxy_port_fd, result->ai_addr, result->ai_addrlen)) < 0) {
    mgmt_elog(stderr, "[bindProxyPort] Unable to bind socket: %d : %s\n", proxy_port, strerror(errno));
    _exit(1);
  }

  freeaddrinfo(result);

  Debug("lm", "[bindProxyPort] Successfully bound proxy port %d\n", proxy_port);

#if !TS_USE_POSIX_CAP
  if (proxy_port < 1024 && euid != 0) {
    if (privBoost == true) {
      if (removeRootPriv(saved_euid) == false) {
        mgmt_elog(stderr, "[bindProxyPort] Unable to reset permissions to euid %d.  Exiting...\n", getuid());
        _exit(1);
      }
    }
  }
#endif
  return proxy_port_fd;
}

void
LocalManager::signalAlarm(int alarm_id, const char *desc, const char *ip)
{
  if (alarm_keeper)
    alarm_keeper->signalAlarm((alarm_t) alarm_id, desc, ip);
}
