/****************************************************************************
 *   Copyright (C) 2006-2010 by Jason Ansel, Kapil Arya, and Gene Cooperman *
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

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include "constants.h"
#include "mtcpinterface.h"
#include "dmtcpaware.h"
#include "syscallwrappers.h"
#include "uniquepid.h"
#include "dmtcpworker.h"
#include "processinfo.h"
#include "protectedfds.h"
#include "sockettable.h"
#include "dmtcpplugin.h"
#include "util.h"

#include "../jalib/jfilesystem.h"
#include "../jalib/jconvert.h"
#include "../jalib/jassert.h"
#include "../jalib/jalloc.h"

#ifdef DEBUG
static int debugEnabled = 1;
#else
static int debugEnabled = 0;
#endif

#ifdef PID_VIRTUALIZATION
//static int pidVirtualizationEnabled = 1;
static int pidVirtualizationEnabled = 0;
#else
static int pidVirtualizationEnabled = 0;
#endif

dmtcp::vector<DmtcpPreResumeUserThreadFunctionPointer>
  dmtcp::preResumeUserThreadFuncs;
dmtcp::vector<DmtcpPreSuspendUserThreadFunctionPointer>
  dmtcp::preSuspendUserThreadFuncs;
static char prctlPrgName[22] = {0};
static void prctlGetProcessName();
static void prctlRestoreProcessName();

static char *_mtcpRestoreArgvStartAddr = NULL;
static void restoreArgvAfterRestart(char* mtcpRestoreArgvStartAddr);
static void unmapRestoreArgv();

static const char* REOPEN_MTCP = ( char* ) 0x1;

static void callbackSleepBetweenCheckpoint(int sec);
static void callbackPreCheckpoint(char **ckptFilename);
static void callbackPostCheckpoint(int isRestart,
                                   char* mtcpRestoreArgvStartAddr);
static int callbackShouldCkptFD(int /*fd*/);
static void callbackWriteCkptPrefix(int fd);
static void callbackRestoreVirtualPidTable();

void callbackHoldsAnyLocks(int *retval);
void callbackPreSuspendUserThread();
void callbackPreResumeUserThread(int is_ckpt, int is_restart);

LIB_PRIVATE MtcpFuncPtrs_t mtcpFuncPtrs;

#ifdef EXTERNAL_SOCKET_HANDLING
static bool delayedCheckpoint = false;
#endif

static void* find_and_open_mtcp_so()
{
#ifndef ANDROID
  dmtcp::string mtcpso = jalib::Filesystem::FindHelperUtility ( "libmtcp.so.1" );
#else
  dmtcp::string mtcpso = jalib::Filesystem::FindHelperUtility ( "libmtcp.so" );
#endif
  void* handle = dlopen ( mtcpso.c_str(), RTLD_NOW );
  JASSERT ( handle != NULL ) ( mtcpso ) (dlerror())
    .Text ( "failed to load libmtcp.so" );
  return handle;
}

// Note that mtcp.so is closed and re-opened (maybe in a different
//   location) at the time of fork.  Do not statically save the
//   return value of get_mtcp_symbol across a fork.
LIB_PRIVATE
void* get_mtcp_symbol ( const char* name )
{
  static void* theMtcpHandle = find_and_open_mtcp_so();

  if ( name == REOPEN_MTCP )
  {
    JTRACE ( "reopening libmtcp.so" ) ( theMtcpHandle );
    //must get ref count down to 0 so it is really unloaded
    for( int i=0; i<MAX_DLCLOSE_MTCP_CALLS; ++i){
      if(dlclose(theMtcpHandle) != 0){
        //failed call means it is unloaded
        JTRACE("dlclose(libmtcp.so) worked");
        break;
      }else{
        JTRACE("dlclose(libmtcp.so) decremented refcount");
      }
    }
    theMtcpHandle = find_and_open_mtcp_so();
    JTRACE ( "reopening libmtcp.so DONE" ) ( theMtcpHandle );
    return 0;
  }

  void* tmp = _real_dlsym ( theMtcpHandle, name );
  JASSERT ( tmp != NULL ) ( name )
    .Text ( "failed to find libmtcp.so symbol for 'name'\n"
            "Maybe try re-compiling MTCP:   (cd mtcp; make clean); make" );

  //JTRACE("looking up libmtcp.so symbol")(name);

  return tmp;
}

static void initializeMtcpFuncPtrs()
{
  mtcpFuncPtrs.init = (mtcp_init_t) get_mtcp_symbol("mtcp_init");
  mtcpFuncPtrs.ok = (mtcp_ok_t) get_mtcp_symbol("mtcp_ok");
  mtcpFuncPtrs.threadiszombie =
    (mtcp_threadiszombie) get_mtcp_symbol("mtcp_threadiszombie");
#ifndef ANDROID
  mtcpFuncPtrs.clone = (mtcp_clone_t) get_mtcp_symbol("__clone");
#else
  mtcpFuncPtrs.clone = (mtcp_clone_t) get_mtcp_symbol("__pthread_clone");
#endif
  mtcpFuncPtrs.fill_in_pthread_id =
    (mtcp_fill_in_pthread_id_t) get_mtcp_symbol("mtcp_fill_in_pthread_id");
  mtcpFuncPtrs.kill_ckpthread =
    (mtcp_kill_ckpthread_t) get_mtcp_symbol("mtcp_kill_ckpthread");
  mtcpFuncPtrs.process_pthread_join =
    (mtcp_process_pthread_join_t) get_mtcp_symbol("mtcp_process_pthread_join");
  mtcpFuncPtrs.init_dmtcp_info =
    (mtcp_init_dmtcp_info_t) get_mtcp_symbol("mtcp_init_dmtcp_info");
  mtcpFuncPtrs.set_callbacks =
    (mtcp_set_callbacks_t) get_mtcp_symbol("mtcp_set_callbacks");
  mtcpFuncPtrs.set_dmtcp_callbacks =
    (mtcp_set_dmtcp_callbacks_t) get_mtcp_symbol("mtcp_set_dmtcp_callbacks");
  mtcpFuncPtrs.printf =
    (mtcp_printf_t) get_mtcp_symbol("mtcp_printf");
  mtcpFuncPtrs.prepare_for_clone =
    (mtcp_prepare_for_clone_t) get_mtcp_symbol("mtcp_prepare_for_clone");
  mtcpFuncPtrs.thread_start =
    (mtcp_thread_start_t) get_mtcp_symbol("mtcp_thread_start");
  mtcpFuncPtrs.thread_return =
    (mtcp_thread_return_t) get_mtcp_symbol("mtcp_thread_return");
}

static void initializeDmtcpInfoInMtcp()
{
  int jassertlog_fd = debugEnabled ? PROTECTED_JASSERTLOG_FD : -1;

  // DMTCP restores working dir only if --checkpoint-open-files invoked.
  // Later, we may offer the user a separate command line option for this.
  int restore_working_directory = getenv(ENV_VAR_CKPT_OPEN_FILES) ? 1 : 0;

  void *clone_fptr = (void*) _real_clone;
  void *sigaction_fptr = (void*) _real_sigaction;
  // FIXME: What if jalib::JAllocDispatcher is undefined?
  void *malloc_fptr = (void*) jalib::JAllocDispatcher::malloc;
  void *free_fptr = (void*) jalib::JAllocDispatcher::free;
  JASSERT(clone_fptr != NULL);
  JASSERT(sigaction_fptr != NULL);
  JASSERT(malloc_fptr != NULL);
  JASSERT(free_fptr != NULL);


  (*mtcpFuncPtrs.init_dmtcp_info) (pidVirtualizationEnabled,
                                   PROTECTED_STDERR_FD,
                                   jassertlog_fd,
                                   restore_working_directory,
                                   clone_fptr,
                                   sigaction_fptr,
                                   malloc_fptr,
                                   free_fptr);

}

void dmtcp::initializeMtcpEngine()
{
  initializeMtcpFuncPtrs();
  initializeDmtcpInfoInMtcp();

  (*mtcpFuncPtrs.set_callbacks)(&callbackSleepBetweenCheckpoint,
                                &callbackPreCheckpoint,
                                &callbackPostCheckpoint,
                                &callbackShouldCkptFD,
                                &callbackWriteCkptPrefix);

  (*mtcpFuncPtrs.set_dmtcp_callbacks)(&callbackRestoreVirtualPidTable,
                                      &callbackHoldsAnyLocks,
                                      &callbackPreSuspendUserThread,
                                      &callbackPreResumeUserThread);

  JTRACE ("Calling mtcp_init");
  mtcpFuncPtrs.init(UniquePid::getCkptFilename(), 0xBadF00d, 1);
  mtcpFuncPtrs.ok();

  JTRACE ( "mtcp_init complete" ) ( UniquePid::getCkptFilename() );
}

static void callbackSleepBetweenCheckpoint ( int sec )
{
  dmtcp::ThreadSync::waitForUserThreadsToFinishPreResumeCB();
  dmtcp_process_event(DMTCP_EVENT_WAIT_FOR_SUSPEND_MSG, NULL);
  dmtcp::DmtcpWorker::instance().waitForStage1Suspend();

  prctlGetProcessName();
  unmapRestoreArgv();

  dmtcp_process_event(DMTCP_EVENT_GOT_SUSPEND_MSG,
                      (void*) dmtcp::ProcessInfo::instance().numThreads());
  // After acquiring this lock, there shouldn't be any
  // allocations/deallocations and JASSERT/JTRACE/JWARNING/JNOTE etc.; the
  // process can deadlock.
  JALIB_CKPT_LOCK();
}

static void callbackPreCheckpoint( char ** ckptFilename )
{
  // All we want to do is unlock the jassert/jalloc locks, if we reset them, it
  // serves the purpose without having a callback.
  // TODO: Check for correctness.
  JALIB_CKPT_UNLOCK();

  dmtcp_process_event(DMTCP_EVENT_START_PRE_CKPT_CB, NULL);

  //now user threads are stopped
  dmtcp::userHookTrampoline_preCkpt();
#ifdef EXTERNAL_SOCKET_HANDLING
  if (dmtcp::DmtcpWorker::instance().waitForStage2Checkpoint() == false) {
    char *nullDevice = (char *) "/dev/null";
    *ckptFilename = nullDevice;
    delayedCheckpoint = true;
  } else
#else
  dmtcp::DmtcpWorker::instance().waitForStage2Checkpoint();
#endif
  *ckptFilename = const_cast<char *>(dmtcp::UniquePid::getCkptFilename());
  JTRACE ( "MTCP is about to write checkpoint image." )(*ckptFilename);

}


static void callbackPostCheckpoint ( int isRestart,
                                     char* mtcpRestoreArgvStartAddr)
{
  if ( isRestart )
  {
    restoreArgvAfterRestart(mtcpRestoreArgvStartAddr);
    prctlRestoreProcessName();

    dmtcp_process_event(DMTCP_EVENT_POST_RESTART, NULL);

    dmtcp::DmtcpWorker::instance().postRestart();
    /* FIXME: There is not need to call sendCkptFilenameToCoordinator() but if
     *        we do not call it, it exposes a bug in dmtcp_coordinator.
     * BUG: The restarting process reconnects to the coordinator and the old
     *      connection is discarded. However, the coordinator doesn't discard
     *      the old connection right away (since it can't detect if the other
     *      end of the socket is closed). It is only discarded after the next
     *      read phase (coordinator trying to read from all the connected
     *      workers) in monitorSockets() is complete.  In this read phase, an
     *      error is recorded on the closed socket and in the next iteration of
     *      verifying the _dataSockets, this socket is closed and the
     *      corresponding entry in _dataSockets is freed.
     *
     *      The problem occurs when some other worker sends a status messages
     *      which should take the computation to the next barrier, but since
     *      the _to_be_disconnected socket is present, the minimum state is not
     *      reached unanimously and hence the coordinator doesn't raise the
     *      barrier.
     *
     *      The bug was observed by Kapil in gettimeofday test program. It can
     *      be seen in 1 out of 3 restart attempts.
     *
     *      The current solution is to send a dummy message to coordinator here
     *      before sending a proper request.
     */
    dmtcp::DmtcpWorker::instance().sendCkptFilenameToCoordinator();
    dmtcp::DmtcpWorker::instance().waitForStage3Refill(isRestart);
    callbackRestoreVirtualPidTable();
  }
  else
  {
#ifdef EXTERNAL_SOCKET_HANDLING
    if ( delayedCheckpoint == false )
#endif
    {
      dmtcp::DmtcpWorker::instance().sendCkptFilenameToCoordinator();
      dmtcp::DmtcpWorker::instance().waitForStage3Refill(isRestart);
      dmtcp::DmtcpWorker::instance().waitForStage4Resume();
      dmtcp_process_event(DMTCP_EVENT_POST_CKPT_RESUME, NULL);
    }

    // Set the process state to RUNNING now, in case a dmtcpaware hook
    //  calls pthread_create, thereby invoking our virtualization.
    dmtcp::WorkerState::setCurrentState( dmtcp::WorkerState::RUNNING );
    // Now everything but user threads are restored.  Call the user hook.
    dmtcp::userHookTrampoline_postCkpt(isRestart);
    // After this, the user threads will be unlocked in mtcp.c and will resume.
  }
}

static int callbackShouldCkptFD ( int /*fd*/ )
{
  //mtcp should never checkpoint file descriptors;  dmtcp will handle it
  return 0;
}

static void callbackWriteCkptPrefix ( int fd )
{
  dmtcp::DmtcpWorker::instance().writeCheckpointPrefix(fd);
  dmtcp_process_event(DMTCP_EVENT_WRITE_CKPT_PREFIX, (void*) (unsigned long) fd);
}

static void callbackRestoreVirtualPidTable()
{
  dmtcp_process_event(DMTCP_EVENT_POST_RESTART_REFILL, NULL);
  dmtcp::DmtcpWorker::instance().waitForStage4Resume();

#ifndef RECORD_REPLAY
  /* This calls setenv() which calls malloc. Since this is only executed on
     restart, that means it there is an extra malloc on replay. Commenting this
     until we have time to fix it. */
  dmtcp::DmtcpWorker::instance().updateCoordinatorHostAndPortEnv();
#endif

  dmtcp_process_event(DMTCP_EVENT_POST_RESTART_RESUME, NULL);

  // Set the process state to RUNNING now, in case a dmtcpaware hook
  //  calls pthread_create, thereby invoking our virtualization.
  dmtcp::WorkerState::setCurrentState( dmtcp::WorkerState::RUNNING );
  // Now everything but user threads are restored.  Call the user hook.
  dmtcp::userHookTrampoline_postCkpt(true);
  // After this, the user threads will be unlocked in mtcp.c and will resume.
}

extern "C" int dmtcp_is_ptracing() __attribute__ ((weak));
void callbackHoldsAnyLocks(int *retval)
{
  /* This callback is useful only for the ptrace plugin currently, but may be
   * used for other stuff as well.
   *
   * This is invoked as the first thing in stopthisthread() routine, which is
   * the signal handler for CKPT signal, to check if the current thread is
   * holding any of the wrapperExecLock or threadCreationLock. If the thread is
   * holding any of these locks, we return from the signal handler and wait for
   * the thread to release the lock. Once the thread has release the last lock,
   * it will send itself the CKPT signal and will return to the signal handler
   * and will proceed normally.
   */

  dmtcp::ThreadSync::unsetOkToGrabLock();
  *retval = dmtcp::ThreadSync::isThisThreadHoldingAnyLocks();
  if (*retval == TRUE) {
    JASSERT(dmtcp_is_ptracing && dmtcp_is_ptracing());
    dmtcp::ThreadSync::setSendCkptSignalOnFinalUnlock();
  }
}

void callbackPreSuspendUserThread()
{
  dmtcp::ThreadSync::incrNumUserThreads();

  dmtcp::vector<DmtcpPreSuspendUserThreadFunctionPointer>::iterator itr;
  for (itr = dmtcp::preSuspendUserThreadFuncs.begin();
       itr != dmtcp::preSuspendUserThreadFuncs.end();
       ++itr) {
    DmtcpPreSuspendUserThreadFunctionPointer resumeFunPtr = *itr;
    resumeFunPtr();
  }

  dmtcp_process_event(DMTCP_EVENT_PRE_SUSPEND_USER_THREAD, NULL);
}

void callbackPreResumeUserThread(int is_ckpt, int is_restart)
{
  DmtcpResumeUserThreadInfo info;
  info.is_ckpt = is_ckpt;
  info.is_restart = is_restart;
  dmtcp_process_event(DMTCP_EVENT_RESUME_USER_THREAD, &info);
  dmtcp::ThreadSync::setOkToGrabLock();
  // This should be the last significant work before returning from this
  // function.
  dmtcp::ThreadSync::processPreResumeCB();

  dmtcp::vector<DmtcpPreResumeUserThreadFunctionPointer>::iterator itr;
  for (itr = dmtcp::preResumeUserThreadFuncs.begin();
       itr != dmtcp::preResumeUserThreadFuncs.end();
       ++itr) {
    DmtcpPreResumeUserThreadFunctionPointer resumeFunPtr = *itr;
    resumeFunPtr(is_ckpt, is_restart);
  }

  // Make a dummy syscall to inform superior of our status before we resume. If
  // ptrace is disabled, this call has no significant effect.
  syscall(DMTCP_FAKE_SYSCALL);
}

void prctlGetProcessName()
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11)
  if (prctlPrgName[0] == '\0') {
    memset(prctlPrgName, 0, sizeof(prctlPrgName));
    strcpy(prctlPrgName, DMTCP_PRGNAME_PREFIX);
    int ret = prctl(PR_GET_NAME, &prctlPrgName[strlen(DMTCP_PRGNAME_PREFIX)]);
    if (ret != -1) {
      JTRACE("prctl(PR_GET_NAME, ...) succeeded") (prctlPrgName);
    } else {
      JASSERT(errno == EINVAL) (JASSERT_ERRNO)
        .Text ("prctl(PR_GET_NAME, ...) failed");
      JTRACE("prctl(PR_GET_NAME, ...) failed. Not supported on this kernel?");
    }
  }
#endif
}

void prctlRestoreProcessName()
{
  // Although PR_SET_NAME has been supported since 2.6.9, we wouldn't use it on
  // kernel < 2.6.11 since we didn't get the process name using PR_GET_NAME
  // which is supported on >= 2.6.11
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11)
    if (prctl(PR_SET_NAME, prctlPrgName) != -1) {
      JTRACE("prctl(PR_SET_NAME, ...) succeeded") (prctlPrgName);
    } else {
      JASSERT(errno == EINVAL) (prctlPrgName) (JASSERT_ERRNO)
        .Text ("prctl(PR_SET_NAME, ...) failed");
      JTRACE("prctl(PR_SET_NAME, ...) failed") (prctlPrgName);
    }
#endif
}

static void restoreArgvAfterRestart(char* mtcpRestoreArgvStartAddr)
{
  /*
   * The addresses where argv of mtcp_restart process starts. /proc/PID/cmdline
   * information is looked up from these addresses.  We observed that the
   * stack-base for mtcp_restart is always 0x7ffffffff000 in 64-bit system and
   * 0xc0000000 in case of 32-bit system.  Once we restore the checkpointed
   * process's memory, we will map the pages ending in these address into the
   * process's memory if they are unused i.e. not mapped by the process (which
   * is true for most processes running with ASLR).  Once we map them, we can
   * put the argv of the checkpointed process in there so that
   * /proc/self/cmdline shows the correct values.
   * Note that if compiled in 32-bit mode '-m32', the stack base address
   * is in still a different location, and so this logic is not valid.
   */
  JASSERT(mtcpRestoreArgvStartAddr != NULL);

  long page_size = sysconf(_SC_PAGESIZE);
  long page_mask = ~(page_size - 1);
  char *startAddr = (char*) ((unsigned long) mtcpRestoreArgvStartAddr & page_mask);

  size_t len;
  len = (dmtcp::ProcessInfo::instance().argvSize() + page_size) & page_mask;

  // Check to verify if any page in the given range is already mmap()'d.
  // It assumes that the given addresses may belong to stack only and if
  // mapped, will have read+write permissions.
  for (size_t i = 0; i < len; i += page_size) {
    int ret = mprotect ((char*) startAddr + i, page_size,
                        PROT_READ | PROT_WRITE);
    if (ret != -1 || errno != ENOMEM) {
      _mtcpRestoreArgvStartAddr = NULL;
      return;
    }
  }

  //None of the pages are mapped -- it is safe to mmap() them
  void *retAddr = mmap((void*) startAddr, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (retAddr != MAP_FAILED) {
    JTRACE("Restoring /proc/self/cmdline")
      (mtcpRestoreArgvStartAddr) (startAddr) (len) (JASSERT_ERRNO) ;
    dmtcp::vector<dmtcp::string> args = jalib::Filesystem::GetProgramArgs();
    char *addr = mtcpRestoreArgvStartAddr;
    // Do NOT change restarted process's /proc/self/cmdline.
    //args[0] = DMTCP_PRGNAME_PREFIX + args[0];
    for ( size_t i=0; i< args.size(); ++i ) {
      if (addr + args[i].length() >= startAddr + len)
        break;
      strcpy(addr, args[i].c_str());
      addr += args[i].length() + 1;
    }
    _mtcpRestoreArgvStartAddr = startAddr;
  } else {
    JTRACE("Unable to restore /proc/self/cmdline") (startAddr) (len) (JASSERT_ERRNO) ;
    _mtcpRestoreArgvStartAddr = NULL;
  }
  return;
}

static void unmapRestoreArgv()
{
  long page_size = sysconf(_SC_PAGESIZE);
  long page_mask = ~(page_size - 1);
  if (_mtcpRestoreArgvStartAddr != NULL) {
    JTRACE("Unmapping previously mmap()'d pages (that were mmap()'d for restoring argv");
    size_t len;
    len = (dmtcp::ProcessInfo::instance().argvSize() + page_size) & page_mask;
    JASSERT(_real_munmap(_mtcpRestoreArgvStartAddr, len) == 0)
      (_mtcpRestoreArgvStartAddr) (len)
      .Text ("Failed to munmap extra pages that were mapped during restart");
  }
}

// FIXME
// Starting here, we can continue with files for mtcpinterface.cpp - Gene

  // This is called by the child process, only, via DmtcpWorker::resetOnFork().
  // We know that no one can send the SIG_CKPT signal, since if the
  //   the coordinator had requested a checkpoint, then either the
  //   the child successfully forked, or the thread of the parent process
  //   seeing the fork is processing the checkpoint signal first.  The
  //   latter case is no problem.  If the child successfully forked, then
  //   the SIG_CKPT sent by the checkpoint thread of the parent process prior
  //   to forking is too late to affect the child.  The checkpoint thread
  //   of the parent process may continue its own checkpointing, but
  //   the child process will not take part.  It's the coordinator's
  //   responsibility to then also send a checkpoint message to the checkpoint
  //   thread of the child.  DOES THE COORDINATOR DO THIS?
  // After a fork, only the child's user thread (which called fork())
  //   exists (and we know it's not our own checkpoint thread).  So, no
  //   thread is listening for a checkpoint command via the socket
  //   from the coordinator, _even_ if the coordinator decided to start
  //   the checkpoint immediately after the fork.  The child can't checkpoint
  //   until we call mtcp_init in the child, as described below.
  //   Note that resetOnFork() is the last thing done by the child before the
  //   fork wrapper returns.
  //   Jason, PLEASE VERIFY THE LOGIC ABOVE.  IT'S FOR THIS REASON, WE
  //   SHOULDN'T NEED delayCheckpointsLock.  Thanks.  - Gene

  // shutdownMtcpEngineOnFork will dlclose the old libmtcp.so and will
  //   dlopen a new libmtcp.so.  DmtcpWorker constructor then calls
  //   initializeMtcpEngine, which will then call mtcp_init.  We must close
  //   the old SIG_CKPT handler prior to this, so that MTCP and mtcp_init()
  //   don't think someone else is using their SIG_CKPT signal.
void dmtcp::shutdownMtcpEngineOnFork()
{
  // Remove our signal handler from our SIG_CKPT
  errno = 0;
  JWARNING (SIG_ERR != _real_signal(dmtcp::DmtcpWorker::determineMtcpSignal(),
                                    SIG_DFL))
           (dmtcp::DmtcpWorker::determineMtcpSignal())
           (JASSERT_ERRNO)
           .Text("failed to reset child's checkpoint signal on fork");
  get_mtcp_symbol ( REOPEN_MTCP );
}

void dmtcp::killCkpthread()
{
  mtcpFuncPtrs.kill_ckpthread();
}
