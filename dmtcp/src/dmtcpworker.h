/****************************************************************************
 *   Copyright (C) 2006-2008 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *   This file is part of the dmtcp/src module of DMTCP (DMTCP:dmtcp/src).  *
 *                                                                          *
 *  DMTCP:dmtcp/src is free software: you can redistribute it and/or        *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP:dmtcp/src is distributed in the hope that it will be useful,      *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#ifndef DMTCPDMTCPWORKER_H
#define DMTCPDMTCPWORKER_H

#include "dmtcpcoordinatorapi.h"
#include "dmtcpalloc.h"
#include  "../jalib/jsocket.h"
#include "../jalib/jalloc.h"
#include "uniquepid.h"
#include "constants.h"
#include "dmtcpmessagetypes.h"
#include "syscallwrappers.h"
#include "threadsync.h"
#include "processinfo.h"

LIB_PRIVATE extern int dmtcp_wrappers_initializing;

void restoreUserLDPRELOAD();

namespace dmtcp
{

  void userHookEarlyCheckpoint();
  void earlyCheckpoint();

  class ConnectionState;

#ifdef EXTERNAL_SOCKET_HANDLING
  class TcpConnectionInfo {
    public:
      TcpConnectionInfo (const ConnectionIdentifier& id,
                       socklen_t& len,
                       struct sockaddr_storage& remote,
                       struct sockaddr_storage& local) {
        _conId      = id;
        _addrlen    = len;
        memcpy ( &_remoteAddr, &remote, len );
        memcpy ( &_localAddr, &local, len );
      }

    ConnectionIdentifier&  conId() { return _conId; }
    socklen_t addrlen() { return _addrlen; }
    struct sockaddr_storage remoteAddr() { return _remoteAddr; }
    struct sockaddr_storage localAddr() { return _localAddr; }

    ConnectionIdentifier    _conId;
    socklen_t               _addrlen;
    struct sockaddr_storage _remoteAddr;
    struct sockaddr_storage _localAddr;
  };
#endif


  class DmtcpWorker : public DmtcpCoordinatorAPI
  {
    public:
#ifdef JALIB_ALLOCATOR
      static void* operator new(size_t nbytes, void* p) { return p; }
      static void* operator new(size_t nbytes) { JALLOC_HELPER_NEW(nbytes); }
      static void  operator delete(void* p) { JALLOC_HELPER_DELETE(p); }
#endif
      static DmtcpWorker& instance();
      const dmtcp::UniquePid& coordinatorId() const;
      jalib::JSocket& coordinatorSocket() { return _coordinatorSocket; }

      void waitForCoordinatorMsg(dmtcp::string signalStr,
                                 DmtcpMessageType type);
      void sendCkptFilenameToCoordinator();
      void waitForStage1Suspend();
#ifdef EXTERNAL_SOCKET_HANDLING
      bool waitForStage2Checkpoint();
      bool waitForStage2bCheckpoint();
      void sendPeerLookupRequest(dmtcp::vector<TcpConnectionInfo>& conInfoTable );
      static bool waitingForExternalSocketsToClose();
#else
      void waitForStage2Checkpoint();
#endif
      void waitForStage3Refill(bool isRestart);
      void waitForStage4Resume();
      void restoreVirtualPidTable();
      void postRestart();
      void updateCoordinatorHostAndPortEnv();

      static void resetOnFork(jalib::JSocket& coordSock);
      void cleanupWorker();

      DmtcpWorker ( bool shouldEnableCheckpointing );
      ~DmtcpWorker();

      static int determineMtcpSignal();

      static void delayCheckpointsLock();
      static void delayCheckpointsUnlock();

      static bool wrapperExecutionLockLock();
      static void wrapperExecutionLockUnlock();
      static bool wrapperExecutionLockLockExcl();
      static void resetLocks();

      static bool threadCreationLockLock();
      static void threadCreationLockUnlock();

      static void waitForThreadsToFinishInitialization();
      static void incrementUninitializedThreadCount();
      static void decrementUninitializedThreadCount();

      static void disableLockAcquisitionForThisThread();
      static bool isThisThreadHoldingAnyLocks();

      static void setExitInProgress() { _exitInProgress = true; };
      static bool exitInProgress() { return _exitInProgress; };
      void interruptCkpthread();

      void writeCheckpointPrefix(int fd);
      void writeTidMaps();

      static void startSynchronize();
      static void waitSynchronize();

    protected:
      void sendUserCommand(char c, int* result = NULL);
    private:
      static DmtcpWorker theInstance;
    private:
      static bool _exitInProgress;
  };
}

#endif