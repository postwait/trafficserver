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

#include "libts.h"
#include "I_Layout.h"
#include "DiagsConfig.h"
#ifdef LOCAL_MANAGER

#include "../mgmt/Main.h"
#define MGMT_PTR       lmgmt
#define DIAGS_LOG_FILE "manager.log"

#else
#include "Main.h"
#include "ProxyConfig.h"
#define MGMT_PTR       pmgmt
#define DIAGS_LOG_FILE "diags.log"

#endif

#include "P_RecCore.h"


//////////////////////////////////////////////////////////////////////////////
//
//      void reconfigure_diags()
//
//      This function extracts the current diags configuration settings from
//      records.config, and rebuilds the Diags data structures.
//
//////////////////////////////////////////////////////////////////////////////



void
DiagsConfig::reconfigure_diags()
{
  int i, e;
  char *p, *dt, *at;
  DiagsConfigState c;
  bool found, all_found;

  static struct
  {
    const char *config_name;
    DiagsLevel level;
  } output_records[] = {
    {
    "proxy.config.diags.output.diag", DL_Diag}, {
    "proxy.config.diags.output.debug", DL_Debug}, {
    "proxy.config.diags.output.status", DL_Status}, {
    "proxy.config.diags.output.note", DL_Note}, {
    "proxy.config.diags.output.warning", DL_Warning}, {
    "proxy.config.diags.output.error", DL_Error}, {
    "proxy.config.diags.output.fatal", DL_Fatal}, {
    "proxy.config.diags.output.alert", DL_Alert}, {
    "proxy.config.diags.output.emergency", DL_Emergency}, {
    NULL, DL_Undefined}
  };

  if (!callbacks_established) {
    register_diags_callbacks();
  }
  ////////////////////////////////////////////
  // extract relevant records.config values //
  ////////////////////////////////////////////

  all_found = true;

  // initial value set to 0 or 1 based on command line tags
  c.enabled[DiagsTagType_Debug] = (diags->base_debug_tags != NULL);
  c.enabled[DiagsTagType_Action] = (diags->base_action_tags != NULL);

  // enabled if records.config set

  e = (int) REC_readInteger("proxy.config.diags.debug.enabled", &found);
  if (e && found)
    c.enabled[DiagsTagType_Debug] = 1;  // implement OR logic
  all_found = all_found && found;

  e = (int) REC_readInteger("proxy.config.diags.action.enabled", &found);
  if (e && found)
    c.enabled[DiagsTagType_Action] = 1; // implement OR logic
  all_found = all_found && found;

  e = (int) REC_readInteger("proxy.config.diags.show_location", &found);
  diags->show_location = ((e && found) ? 1 : 0);
  all_found = all_found && found;

  // read output routing values
  for (i = 0;; i++) {
    const char *record_name = output_records[i].config_name;
    DiagsLevel l = output_records[i].level;

    if (!record_name)
      break;
    p = REC_readString(record_name, &found);
    all_found = all_found && found;
    if (found) {
      parse_output_string(p, &(c.outputs[l]));
      ats_free(p);
    } else {
      SrcLoc loc(__FILE__, __FUNCTION__, __LINE__);
      diags->print(NULL, DL_Error, NULL, &loc, "can't find config variable '%s'\n", record_name);
    }
  }

  p = REC_readString("proxy.config.diags.debug.tags", &found);
  dt = (found ? p : NULL);      // NOTE: needs to be freed
  all_found = all_found && found;

  p = REC_readString("proxy.config.diags.action.tags", &found);
  at = (found ? p : NULL);      // NOTE: needs to be freed
  all_found = all_found && found;

  ///////////////////////////////////////////////////////////////////
  // if couldn't read all values, return without changing config,  //
  // otherwise rebuild taglists and change the diags config values //
  ///////////////////////////////////////////////////////////////////

  if (!all_found) {
    SrcLoc loc(__FILE__, __FUNCTION__, __LINE__);
    diags->print(NULL, DL_Error, NULL, &loc, "couldn't fetch all proxy.config.diags values");
  } else {
    //////////////////////////////
    // clear out old tag tables //
    //////////////////////////////

    diags->deactivate_all(DiagsTagType_Debug);
    diags->deactivate_all(DiagsTagType_Action);

    //////////////////////////////////////////////////////////////////////
    // add new tag tables from records.config or command line overrides //
    //////////////////////////////////////////////////////////////////////

    diags->activate_taglist((diags->base_debug_tags ? diags->base_debug_tags : dt), DiagsTagType_Debug);
    diags->activate_taglist((diags->base_action_tags ? diags->base_action_tags : at), DiagsTagType_Action);

    ////////////////////////////////////
    // change the diags config values //
    ////////////////////////////////////
#if !defined(__GNUC__)
    diags->config = c;
#else
    memcpy(((void *) &diags->config), ((void *) &c), sizeof(DiagsConfigState));
#endif
    diags->print(NULL, DL_Note, NULL, NULL, "updated diags config");
  }

  ////////////////////////////////////
  // free the record.config strings //
  ////////////////////////////////////
  ats_free(dt);
  ats_free(at);
}


//////////////////////////////////////////////////////////////////////////////
//
//      static void *diags_config_callback(void *opaque_token, void *data)
//
//      This is the records.config registration callback that is called
//      when any diags value is changed.  Each time a diags value changes
//      the entire diags state is reconfigured.
//
//////////////////////////////////////////////////////////////////////////////
static int
diags_config_callback(const char *name, RecDataT data_type, RecData data, void *opaque_token)
{
  NOWARN_UNUSED(name);
  NOWARN_UNUSED(data_type);
  NOWARN_UNUSED(data);
  DiagsConfig *diagsConfig;

  diagsConfig = (DiagsConfig *) opaque_token;
  ink_assert(diags->magic == DIAGS_MAGIC);
  diagsConfig->reconfigure_diags();
  return (0);
}






//////////////////////////////////////////////////////////////////////////////
//
//      void Diags::parse_output_string(char *s, DiagsModeOutput *o)
//
//      This routine converts a diags outpur routing string <s> to the
//      internal DiagsModeOutput structure.  Currently there are 4 possible
//      routing destinations:
//              O  stdout
//              E  stderr
//              S  syslog
//              L  diags.log
//
//////////////////////////////////////////////////////////////////////////////

void
DiagsConfig::parse_output_string(char *s, DiagsModeOutput * o)
{
  o->to_stdout = (s && strchr(s, 'O'));
  o->to_stderr = (s && strchr(s, 'E'));
  o->to_syslog = (s && strchr(s, 'S'));
  o->to_diagslog = (s && strchr(s, 'L'));
}


//////////////////////////////////////////////////////////////////////////////
//
//      void Diags::config_norecords()
//
//      Builds the Diags data structures based on the command line values
//        it does not use any of the records based config variables
//
//////////////////////////////////////////////////////////////////////////////
void
DiagsConfig::config_diags_norecords()
{
  DiagsConfigState c;

  //////////////////////////////
  // clear out old tag tables //
  //////////////////////////////
  diags->deactivate_all(DiagsTagType_Debug);
  diags->deactivate_all(DiagsTagType_Action);

  //////////////////////////////////////////////////////////////////////
  // add new tag tables from command line overrides only              //
  //////////////////////////////////////////////////////////////////////

  if (diags->base_debug_tags) {
    diags->activate_taglist(diags->base_debug_tags, DiagsTagType_Debug);
    c.enabled[DiagsTagType_Debug] = 1;
  } else {
    c.enabled[DiagsTagType_Debug] = 0;
  }

  if (diags->base_action_tags) {
    diags->activate_taglist(diags->base_action_tags, DiagsTagType_Action);
    c.enabled[DiagsTagType_Action] = 1;
  } else {
    c.enabled[DiagsTagType_Action] = 0;
  }

#if !defined(__GNUC__)
  diags->config = c;
#else
  memcpy(((void *) &diags->config), ((void *) &c), sizeof(DiagsConfigState));
#endif
}

void
DiagsConfig::RegisterDiagConfig()
{
  RecRegisterConfigInt(RECT_CONFIG, "proxy.config.diags.debug.enabled", 0, RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.debug.tags", "", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigInt(RECT_CONFIG, "proxy.config.diags.action.enabled", 0, RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.action.tags", "", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigInt(RECT_CONFIG, "proxy.config.diags.show_location", 0, RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.diag", "E", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.debug", "E", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.status", "S", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.note", "S", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.warning", "S", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.error", "SE", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.fatal", "SE", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.alert", "SE", RECU_NULL, RECC_NULL, NULL);
  RecRegisterConfigString(RECT_CONFIG, "proxy.config.diags.output.emergency", "SE", RECU_NULL, RECC_NULL, NULL);
}


DiagsConfig::DiagsConfig(char *bdt, char *bat, bool use_records)
{
  char diags_logpath[PATH_NAME_MAX + 1];
  callbacks_established = false;
  diags_log_fp = (FILE *) NULL;
  diags = NULL;

  ////////////////////////////////////////////////////////////////////
  //  If we aren't using the manager records for configuation       //
  //   just build the tables based on command line parameters and   //
  //   exit                                                         //
  ////////////////////////////////////////////////////////////////////

  if (!use_records) {
    diags = NEW(new Diags(bdt, bat, NULL));
    config_diags_norecords();
    return;
  }
  ////////////////////////
  // open the diags log //
  ////////////////////////

  if (access(system_log_dir, R_OK) == -1) {
    REC_ReadConfigString(diags_logpath, "proxy.config.log.logfile_dir", PATH_NAME_MAX);
    Layout::get()->relative(system_log_dir, PATH_NAME_MAX, diags_logpath);

    if (access(system_log_dir, R_OK) == -1) {
      fprintf(stderr,"unable to access() log dir'%s': %d, %s\n",
              system_log_dir, errno, strerror(errno));
      fprintf(stderr,"please set 'proxy.config.log.logfile_dir'\n");
      _exit(1);
    }
  }
  ink_filepath_make(diags_logpath, sizeof(diags_logpath),
                    system_log_dir, DIAGS_LOG_FILE);

  // open write append
  // diags_log_fp = fopen(diags_logpath,"w");
  diags_log_fp = fopen(diags_logpath, "a+");
  if (diags_log_fp) {
    int status;
    status = setvbuf(diags_log_fp, NULL, _IOLBF, 512);
    if (status != 0) {
      fclose(diags_log_fp);
      diags_log_fp = NULL;
    }
  }

  diags = NEW(new Diags(bdt, bat, diags_log_fp));
  if (diags_log_fp == NULL) {
    SrcLoc loc(__FILE__, __FUNCTION__, __LINE__);

    diags->print(NULL, DL_Warning, NULL, &loc,
                 "couldn't open diags log file '%s', " "will not log to this file", diags_logpath);
  }
  diags->print(NULL, DL_Status, "STATUS", NULL, "opened %s", diags_logpath);

  register_diags_callbacks();

  reconfigure_diags();

}


//////////////////////////////////////////////////////////////////////////////
//
//      void DiagsConfig::register_diags_callbacks()
//
//      set up management callbacks to update diags on every change ---   //
//      right now, this system kind of sucks, we rebuild the tag tables //
//      from scratch for *every* proxy.config.diags value that changed; //
//      dgourley is looking into changing the management API to provide //
//      a callback each time records.config changed, possibly better.   //
//
//////////////////////////////////////////////////////////////////////////////
void
DiagsConfig::register_diags_callbacks()
{

  static const char *config_record_names[] = {
    "proxy.config.diags.debug.enabled",
    "proxy.config.diags.debug.tags",
    "proxy.config.diags.action.enabled",
    "proxy.config.diags.action.tags",
    "proxy.config.diags.show_location",
    "proxy.config.diags.output.diag",
    "proxy.config.diags.output.debug",
    "proxy.config.diags.output.status",
    "proxy.config.diags.output.note",
    "proxy.config.diags.output.warning",
    "proxy.config.diags.output.error",
    "proxy.config.diags.output.fatal",
    "proxy.config.diags.output.alert",
    "proxy.config.diags.output.emergency",
    NULL
  };

  bool total_status = true;
  bool status;
  int i;
  void *o = (void *) this;

  // set triggers to call same callback for any diag config change
  for (i = 0; config_record_names[i] != NULL; i++) {
    status = (REC_RegisterConfigUpdateFunc(config_record_names[i], diags_config_callback, o) == REC_ERR_OKAY);
    if (!status) {
      diags->print(NULL, DL_Warning, NULL, NULL,
                   "couldn't register variable '%s', is records.config up to date?", config_record_names[i]);
    }
    total_status = total_status && status;
  }

  if (total_status == FALSE) {
    diags->print(NULL, DL_Error, NULL, NULL, "couldn't setup all diags callbacks, diagnostics may misbehave");
    callbacks_established = false;
  } else {
    callbacks_established = true;
  }

}

DiagsConfig::~DiagsConfig()
{
  if (diags_log_fp) {
    fclose(diags_log_fp);
    diags_log_fp = NULL;
  }
  delete diags;
}
