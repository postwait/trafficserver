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

/*****************************************************************************
 * Filename: EventControlMain.cc
 * Purpose: Handles all event requests from the user.
 * Created: 01/08/01
 * Created by: lant
 *
 ***************************************************************************/

#include "libts.h"
#include "LocalManager.h"
#include "MgmtSocket.h"
#include "EventControlMain.h"
#include "CoreAPI.h"
#include "NetworkUtilsLocal.h"
#include "NetworkUtilsDefs.h"


// variables that are very important
ink_mutex mgmt_events_lock;
LLQ *mgmt_events;
InkHashTable *accepted_clients; // list of all accepted client connections

/*********************************************************************
 * new_event_client
 *
 * purpose: creates a new EventClientT and return pointer to it
 * input: None
 * output: EventClientT
 * note: None
 *********************************************************************/
EventClientT *
new_event_client()
{
  EventClientT *ele = (EventClientT *)ats_malloc(sizeof(EventClientT));

  // now set the alarms registered section
  for (int i = 0; i < NUM_EVENTS; i++)
    ele->events_registered[i] = 0;

  ele->adr = (struct sockaddr *)ats_malloc(sizeof(struct sockaddr));
  return ele;
}

/*********************************************************************
 * delete_event_client
 *
 * purpose: frees memory allocated for an EventClientT
 * input: EventClientT
 * output: None
 * note: None
 *********************************************************************/
void
delete_event_client(EventClientT * client)
{
  if (client) {
    ats_free(client->adr);
    ats_free(client);
  }
  return;
}

/*********************************************************************
 * remove_event_client
 *
 * purpose: removes the EventClientT from the specified hashtable; includes
 *          removing the binding and freeing the ClientT
 * input: client - the ClientT to remove
 * output:
 *********************************************************************/
void
remove_event_client(EventClientT * client, InkHashTable * table)
{
  // close client socket
  close_socket(client->sock_info.fd);       // close client socket

  // remove client binding from hash table
  ink_hash_table_delete(table, (char *) &client->sock_info.fd);

  // free ClientT
  delete_event_client(client);

  return;
}

/*********************************************************************
 * init_mgmt_events
 *
 * purpose: initializes the mgmt_events queue which is intended to hold
 *          TM events.
 * input:
 * output: TS_ERR_xx
 * note: None
 *********************************************************************/
TSError
init_mgmt_events()
{
  int ret;

  ret = ink_mutex_init(&mgmt_events_lock, "mgmt_event_notice");

  if (ret)
    return TS_ERR_SYS_CALL;

  // initialize queue
  mgmt_events = create_queue();
  if (!mgmt_events) {
    ink_mutex_destroy(&mgmt_events_lock);
    return TS_ERR_SYS_CALL;
  }

  return TS_ERR_OKAY;
}


/*********************************************************************
 * delete_mgmt_events
 *
 * purpose: frees the mgmt_events queue.
 * input:
 * output: None
 * note: None
 *********************************************************************/
void
delete_mgmt_events()
{
  // obtain lock
  ink_mutex_acquire(&mgmt_events_lock);

  // delete the queue associated with the queue of events
  delete_event_queue(mgmt_events);

  // release it
  ink_mutex_release(&mgmt_events_lock);

  // kill lock
  ink_mutex_destroy(&mgmt_events_lock);

  delete_queue(mgmt_events);

  return;
}

/*********************************************************************
 * delete_event_queue
 *
 * purpose: frees queue where the elements are of type TSEvent* 's
 * input: LLQ * q - a queue with entries of TSEvent*'s
 * output: None
 * note: None
 *********************************************************************/
void
delete_event_queue(LLQ * q)
{
  if (!q)
    return;

  // now for every element, dequeue and free
  TSEvent *ele;

  while (!queue_is_empty(q)) {
    ele = (TSEvent *) dequeue(q);
    ats_free(ele);
  }

  delete_queue(q);
  return;
}

/*********************************************************************
 * apiEventCallback
 *
 * purpose: callback function registered with alarm processor so that
 *          each time alarm is signalled, can enqueue it in the mgmt_events
 *          queue
 * input:
 * output: None
 * note: None
 *********************************************************************/
void
apiEventCallback(alarm_t newAlarm, char *ip, char *desc)
{
  NOWARN_UNUSED(ip);
  // create an TSEvent
  // addEvent(new_alarm, ip, desc) // adds event to mgmt_events
  TSEvent *newEvent;

  newEvent = TSEventCreate();
  newEvent->id = newAlarm;
  newEvent->name = get_event_name(newEvent->id);
  //newEvent->ip   = ats_strdup(ip);
  if (desc)
    newEvent->description = ats_strdup(desc);
  else
    newEvent->description = ats_strdup("None");

  //add it to the mgmt_events list
  ink_mutex_acquire(&mgmt_events_lock);
  enqueue(mgmt_events, newEvent);
  ink_mutex_release(&mgmt_events_lock);

  return;
}

/*********************************************************************
 * event_callback_main
 *
 * This function is run as a thread in WebIntrMain.cc that listens on a
 * specified socket. It loops until Traffic Manager dies.
 * In the loop, it just listens on a socket, ready to accept any connections,
 * until receives a request from the remote API client. Parse the request
 * to determine which CoreAPI call to make.
 *********************************************************************/
void *
event_callback_main(void *arg)
{
  int ret;
  OpType op_t;                  // what kind of operation?
  int *socket_fd;
  int con_socket_fd;            // main socket for listening to new connections
  char *req;                    // the request msg sent over from client

  socket_fd = (int *) arg;
  con_socket_fd = *socket_fd;   // the socket for event callbacks

  Debug("event", "[event_callback_main] listen on socket = %d\n", con_socket_fd);

  // initialize queue for accepted con
  accepted_clients = ink_hash_table_create(InkHashTableKeyType_Word);
  if (!accepted_clients) {
    return NULL;
  }
  // initialize queue for holding mgmt events
  if ((ret = init_mgmt_events()) != TS_ERR_OKAY) {
    ink_hash_table_destroy(accepted_clients);
    return NULL;
  }
  // register callback with alarms processor
  lmgmt->alarm_keeper->registerCallback(apiEventCallback);

  // now we can start listening, accepting connections and servicing requests
  int new_con_fd;               // new connection fd when socket accepts connection

  fd_set selectFDs;             // for select call
  InkHashTableEntry *con_entry; // used to obtain fd to alarms mapping
  EventClientT *client_entry;   // an entry of fd to alarms mapping
  InkHashTableIteratorState con_state;  // used to iterate through hash table
  int fds_ready;                // return value for select go here
  struct timeval timeout;
  int addr_len = (sizeof(struct sockaddr));

  while (1) {
    // LINUX fix: to prevent hard-spin reset timeout on each loop
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    FD_ZERO(&selectFDs);

    if (con_socket_fd >= 0) {
      FD_SET(con_socket_fd, &selectFDs);
      Debug("event", "[event_callback_main] add fd %d to select set\n", con_socket_fd);
    }
    // see if there are more fd to set
    con_entry = ink_hash_table_iterator_first(accepted_clients, &con_state);

    // iterate through all entries in hash table
    while (con_entry) {
      client_entry = (EventClientT *) ink_hash_table_entry_value(accepted_clients, con_entry);
      if (client_entry->sock_info.fd >= 0) {    // add fd to select set
        FD_SET(client_entry->sock_info.fd, &selectFDs);
      }
      con_entry = ink_hash_table_iterator_next(accepted_clients, &con_state);
    }

    // select call - timeout is set so we can check events at regular intervals
    fds_ready = mgmt_select(FD_SETSIZE, &selectFDs, (fd_set *) NULL, (fd_set *) NULL, &timeout);

    // check return
    if (fds_ready > 0) {
      // we got connections or requests!

      // first check for connections!
      if (con_socket_fd >= 0 && FD_ISSET(con_socket_fd, &selectFDs)) {
        fds_ready--;

        // create a new instance of the fd to alarms registered mapping
        EventClientT *new_client_con = new_event_client();

        if (!new_client_con) {
          //Debug ("TS_Control_Main", "can't create new EventClientT for new connection\n");
        } else {
          // accept connection
          new_con_fd = mgmt_accept(con_socket_fd, new_client_con->adr, &addr_len);
          new_client_con->sock_info.fd = new_con_fd;
          new_client_con->sock_info.SSLcon = NULL;
          ink_hash_table_insert(accepted_clients, (char *) &new_client_con->sock_info.fd, new_client_con);
          Debug("event", "[event_callback_main] Accept new connection: fd=%d\n", new_con_fd);
        }
      }                         // end if (new_con_fd >= 0 && FD_ISSET(new_con_fd, &selectFDs))

      // some other file descriptor; for each one, service request
      if (fds_ready > 0) {      // RECEIVED A REQUEST from remote API client
        // see if there are more fd to set - iterate through all entries in hash table
        con_entry = ink_hash_table_iterator_first(accepted_clients, &con_state);
        while (con_entry) {
          client_entry = (EventClientT *) ink_hash_table_entry_value(accepted_clients, con_entry);
          // got information check
          if (client_entry->sock_info.fd && FD_ISSET(client_entry->sock_info.fd, &selectFDs)) {
            // SERVICE REQUEST - read the op and message into a buffer
            // clear the fields first
            op_t = UNDEFINED_OP;
            ret = preprocess_msg(client_entry->sock_info, (OpType *) & op_t, &req);
            if (ret == TS_ERR_NET_READ || ret == TS_ERR_NET_EOF) {    // preprocess_msg FAILED!
              Debug("event", "[event_callback_main] preprocess_msg FAILED; skip! \n");
              remove_event_client(client_entry, accepted_clients);
              con_entry = ink_hash_table_iterator_next(accepted_clients, &con_state);
              continue;
            }
            // determine which handler function to call based on operation
            switch (op_t) {

            case EVENT_REG_CALLBACK:
              handle_event_reg_callback(client_entry, req);
              ats_free(req);     // free the request allocated by preprocess_msg
              if (ret == TS_ERR_NET_WRITE || ret == TS_ERR_NET_EOF) {
                Debug("event", "[event_callback_main] ERROR: handle_event_reg_callback\n");
                remove_event_client(client_entry, accepted_clients);
                con_entry = ink_hash_table_iterator_next(accepted_clients, &con_state);
                continue;
              }
              break;

            case EVENT_UNREG_CALLBACK:

              handle_event_unreg_callback(client_entry, req);
              ats_free(req);     // free the request allocated by preprocess_msg
              if (ret == TS_ERR_NET_WRITE || ret == TS_ERR_NET_EOF) {
                Debug("event", "[event_callback_main] ERROR: handle_event_unreg_callback\n");
                remove_event_client(client_entry, accepted_clients);
                con_entry = ink_hash_table_iterator_next(accepted_clients, &con_state);
                continue;
              }
              break;

            default:
              break;

            }                   // end switch (op_t)

          }                     // end if(client_entry->sock_info.fd && FD_ISSET(client_entry->sock_info.fd, &selectFDs))

          con_entry = ink_hash_table_iterator_next(accepted_clients, &con_state);
        }                       // end while (con_entry)
      }                         // end if (fds_ready > 0)

    }                           // end if (fds_ready > 0)

    // ------------ service loop is done, check for events now -------------
    // for each event in the mgmt_events list, uses the event id to check the
    // events_registered queue for each client connection to see if that client
    // has a callback registered for that event_id

    TSEvent *event;

    if (!mgmt_events || queue_is_empty(mgmt_events)) {  //no events to process
      //fprintf(stderr, "[event_callback_main] NO EVENTS TO PROCESS\n");
      Debug("event", "[event_callback_main] NO EVENTS TO PROCESS\n");
      continue;
    }
    // iterate through each event in mgmt_events
    while (!queue_is_empty(mgmt_events)) {
      ink_mutex_acquire(&mgmt_events_lock);     //acquire lock
      event = (TSEvent *) dequeue(mgmt_events);        // get what we want
      ink_mutex_release(&mgmt_events_lock);     // release lock

      if (!event)
        continue;

      //fprintf(stderr, "[event_callback_main] have an EVENT TO PROCESS\n");

      // iterate through all entries in hash table, if any
      con_entry = ink_hash_table_iterator_first(accepted_clients, &con_state);
      while (con_entry) {
        client_entry = (EventClientT *) ink_hash_table_entry_value(accepted_clients, con_entry);
        if (client_entry->events_registered[event->id]) {
          ret = send_event_notification(client_entry->sock_info, event);
          if (ret != TS_ERR_OKAY) {    // send_event_notification failed!
            Debug("event", "sending even notification to fd [%d] failed.\n", client_entry->sock_info);
          }
        }
        // get next client connection, if any
        con_entry = ink_hash_table_iterator_next(accepted_clients, &con_state);
      }                         // end while(con_entry)

      // now we can delete the event
      //fprintf(stderr, "[event_callback_main] DELETE EVENT\n");
      TSEventDestroy(event);
    }                           // end while (!queue_is_empty)

  }                             // end while (1)

  // delete tables
  delete_mgmt_events();

  // iterate through hash table; close client socket connections and remove entry
  con_entry = ink_hash_table_iterator_first(accepted_clients, &con_state);
  while (con_entry) {
    client_entry = (EventClientT *) ink_hash_table_entry_value(accepted_clients, con_entry);
    if (client_entry->sock_info.fd >= 0) {
      close_socket(client_entry->sock_info.fd);     // close socket
    }
    ink_hash_table_delete(accepted_clients, (char *) &client_entry->sock_info.fd);      // remove binding
    delete_event_client(client_entry);  // free ClientT
    con_entry = ink_hash_table_iterator_next(accepted_clients, &con_state);
  }
  // all entries should be removed and freed already
  ink_hash_table_destroy(accepted_clients);

  ink_thread_exit(NULL);
  return NULL;
}

/*-------------------------------------------------------------------------
                             HANDLER FUNCTIONS
 --------------------------------------------------------------------------*/

/**************************************************************************
 * handle_event_reg_callback
 *
 * purpose: handles request to register a callback for a specific event (or all events)
 * input: client - the client currently reading the msg from
 *        req    - the event_name
 * output: TS_ERR_xx
 * note: the req should be the event name; does not send a reply to client
 *************************************************************************/
TSError
handle_event_reg_callback(EventClientT * client, char *req)
{
  // mark the specified alarm as "wanting to be notified" in the client's alarm_registered list
  if (req == NULL) {            // mark all alarms
    for (int i = 0; i < NUM_EVENTS; i++) {
      client->events_registered[i] = true;
    }
  } else {
    int id = get_event_id(req); // req == event_name
    if (id < 0) {
      return TS_ERR_FAIL;
    }
    client->events_registered[id] = true;
  }

  return TS_ERR_OKAY;
}

/**************************************************************************
 * handle_event_unreg_callback
 *
 * purpose: handles request to unregister a callback for a specific event (or all events)
 * input: client - the client currently reading the msg from
 *        req    - the event_name
 * output: TS_ERR_xx
 * note: the req should be the event name; does not send reply to client
 *************************************************************************/
TSError
handle_event_unreg_callback(EventClientT * client, char *req)
{
  // mark the specified alarm as "wanting to be notified" in the client's alarm_registered list
  if (req == NULL) {            // mark all alarms
    for (int i = 0; i < NUM_EVENTS; i++) {
      client->events_registered[i] = false;
    }
  } else {
    int id = get_event_id(req); // req == event_name
    if (id < 0) {
      return TS_ERR_FAIL;
    }
    client->events_registered[id] = false;
  }

  return TS_ERR_OKAY;
}
