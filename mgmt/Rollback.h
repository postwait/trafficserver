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

#ifndef _ROLLBACK_H_
#define _ROLLBACK_H_

/****************************************************************************
 *
 *  Rollback.h - Interface for class to allow rollback of configuration
 *                  files
 *
 *
 ****************************************************************************/

#include "ink_platform.h"
#include "ink_mutex.h"
#include "TextBuffer.h"
#include "ink_bool.h"
#include "ink_assert.h"
#include "List.h"

#define ACTIVE_VERSION 0
#define INVALID_VERSION -1

typedef int version_t;

enum RollBackCodes
{ OK_ROLLBACK, FILE_NOT_FOUND_ROLLBACK,
  VERSION_NOT_CURRENT_ROLLBACK, SYS_CALL_ERROR_ROLLBACK,
  INVALID_VERSION_ROLLBACK
};

class ExpandingArray;

// Stores info about a backup version
//   Can be put in to List.h lists
struct versionInfo
{
  version_t version;
  time_t modTime;
  LINK(versionInfo, link);
};

//
//  class Rollback
//
//  public functions
//
//  _ml functions assume the callee is handling locking issues
//    via acquireLock() and releaseLock().  The non _ml
//    simply grab the lock, call the corresponding _ml function,
//    and then release the lock
//
//  removeVersion(version_t ) - removes the specified version from the
//    configuration directory
//
//  revertToVersion(version_t) - rolls the active version to a new file
//    The specified version of the file is copied to the active version
//
//  getVersion(version_t version, textBuffer** buffer, version_t) -
//    creates a new textBuffer that contains the contents of the specified
//    version.  CALLEE MUST DELETE the buffer
//
//  updateVersion(textBuffer* buf, version_t basedOn) - checks to
//    if basedOn is the current version.  If it is not, the update
//    rejected.  If it is current, the active file is versioned and
//    the contents of buf become the new active file. newVersion tells us what
//    the new version number should be.  -1 means the next in sequence
//
//  forceUpdate(textBuffer* buf, version_t) - Does not check is the new version
//    is based on the current version, which can lead to data loss.  versions
//    the active file and places the contents of buf into the active file
//
//  getCurrentVersion() - returns the current version number.  Unless the
//    callee was acquired the fileAccessLock, the return value only represents
//    a snap shot in time
//
//  numberOfVersions() - returns the number of versions in the config dir.
//    Unless the callee was acquired the fileAccessLock, the return value
//    only represents a snap shot in time
//
//  checkForUserUpdate() - compares the last known modification time
//    of the active version of the file with that files current modification
//    time.  Returns true if the file has been chaged manually or false
//    if it hasn't
//
//  versionTimeStamp(version_t) - returns the modification time (mtime)
//    of the version passed in.  If the version is not foundl, -1 is
//    returned
//
//  findVersions(ExpandingArray* listNames) - scans the config directory for
//    all versions of the file.  If listNames is not NULL, pointers to versionInfo
//    structures are inserted into it.  If is the callee's responsibility
//    to ats_free the versionInfo structures.  They are allocated by ats_malloc
//
// private functions
//
//  CURRENT_VERSION means the active version.  The active version does not
//    have _version appended to its name.  All prior versions are stored
//    as fileName_version.  Calling file operations with CURRENT_VERSION
//    and this->currentVersion have different meanings.  this->currentVersion
//    refers to a file with an _version which does not exist for the active
//    version.
//
//  findVersions() - scans the configuration directory and returns
//    the highest version number encountered
//
//  openFile(version_t version, int oflags) - a wrapper for open
//    opens a file based on version number
//
//  statFile(version_t, struct stat*) - a wrapper for stat that
//    that stats the specified version
//
//  createPathStr(version_t) - creates a string to the specified
//    version of the file.  CALLEE DELETES storage
//
//  internalUpdate(textBuffer*, version_t) - does the really work of the
//    public update functions.  newVersion tells us what the new
//    version number should be.  -1 means the next in sequence

class Rollback
{
public:
  Rollback(const char *baseFileName, bool root_access_needed);
   ~Rollback();

  // Manual take out of lock required
  void acquireLock()
  {
    ink_mutex_acquire(&fileAccessLock);
  };
  void releaseLock()
  {
    ink_mutex_release(&fileAccessLock);
  };
  RollBackCodes removeVersion_ml(version_t version);
  RollBackCodes revertToVersion_ml(version_t version);
  RollBackCodes getVersion_ml(version_t version, textBuffer ** buffer);
  RollBackCodes updateVersion_ml(textBuffer * buf, version_t basedOn,
                                 version_t newVersion = -1, bool notifyChange = true);
  RollBackCodes forceUpdate_ml(textBuffer * buf, version_t newVersion = -1);
  version_t findVersions_ml(ExpandingArray * listNames);
  version_t findVersions_ml(Queue<versionInfo> &q);
  time_t versionTimeStamp_ml(version_t version);
  version_t extractVersionInfo(ExpandingArray * listNames, const char *testFileName);

  // Automatically take out lock
  bool checkForUserUpdate();
  RollBackCodes removeVersion(version_t version);
  RollBackCodes revertToVersion(version_t version);
  RollBackCodes getVersion(version_t version, textBuffer ** buffer);
  RollBackCodes updateVersion(textBuffer * buf, version_t basedOn, version_t newVersion = -1, bool notifyChange = true);
  RollBackCodes forceUpdate(textBuffer * buf, version_t newVersion = -1);
  version_t findVersions(ExpandingArray *);
  time_t versionTimeStamp(version_t version);
  int statVersion(version_t, struct stat *buf);

  // Lock not necessary since these are only valid for a
  //  snap shot in time
  version_t getCurrentVersion()
  {
    return currentVersion;
  };
  int numberOfVersions()
  {
    return numVersions;
  };

  // Not file based so no lock necessary
  const char *getBaseName()
  {
    return fileName;
  };

private:
  Rollback(const Rollback &);
  int openFile(version_t version, int oflags, int *errnoPtr = NULL);
  int closeFile(int fd);
  int statFile(version_t version, struct stat *buf);
  char *createPathStr(version_t version);
  RollBackCodes internalUpdate(textBuffer * buf, version_t newVersion, bool notifyChange = true);
  char *configDir;
  ink_mutex fileAccessLock;
  char *fileName;
  size_t fileNameLen;
  bool root_access_needed;
  version_t currentVersion;
  time_t fileLastModified;
  int numVersions;
  int numberBackups;
  Queue<versionInfo> versionQ;       // stores the backup version info
};

// qSort comptable function to sort versionInfo*
//   based on version number
int versionCmp(const void *i1, const void *i2);
#endif
