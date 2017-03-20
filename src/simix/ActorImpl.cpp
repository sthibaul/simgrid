/* Copyright (c) 2007-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <exception>
#include <functional>
#include <string>
#include <utility>

#include <boost/range/algorithm.hpp>

#include <xbt/functional.hpp>
#include <xbt/ex.hpp>
#include <xbt/sysdep.h>
#include <xbt/log.h>
#include <xbt/dict.h>

#include <simgrid/s4u/host.hpp>

#include <mc/mc.h>

#include "smx_private.h"
#include "src/kernel/activity/SynchroIo.hpp"
#include "src/kernel/activity/SynchroRaw.hpp"
#include "src/kernel/activity/SynchroSleep.hpp"
#include "src/mc/mc_replay.h"
#include "src/mc/remote/Client.hpp"
#include "src/msg/msg_private.h"
#include "src/surf/cpu_interface.hpp"
#include "src/surf/surf_interface.hpp"

#ifdef HAVE_SMPI
#include "src/smpi/private.h"
#endif

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(simix_process, simix, "Logging specific to SIMIX (process)");

unsigned long simix_process_maxpid = 0;

/** Increase the refcount for this process */
smx_actor_t SIMIX_process_ref(smx_actor_t process)
{
  if (process != nullptr)
    intrusive_ptr_add_ref(process);
  return process;
}

/** Decrease the refcount for this process */
void SIMIX_process_unref(smx_actor_t process)
{
  if (process != nullptr)
    intrusive_ptr_release(process);
}

/**
 * \brief Returns the current agent.
 *
 * This functions returns the currently running SIMIX process.
 *
 * \return The SIMIX process
 */
smx_actor_t SIMIX_process_self()
{
  smx_context_t self_context = SIMIX_context_self();

  return (self_context != nullptr) ? self_context->process() : nullptr;
}

/**
 * \brief Returns whether a process has pending asynchronous communications.
 * \return true if there are asynchronous communications in this process
 */
int SIMIX_process_has_pending_comms(smx_actor_t process) {

  return process->comms.size() > 0;
}

/**
 * \brief Moves a process to the list of processes to destroy.
 */
void SIMIX_process_cleanup(smx_actor_t process)
{
  XBT_DEBUG("Cleanup process %s (%p), waiting synchro %p",
      process->name.c_str(), process, process->waiting_synchro);

  process->finished = true;
  SIMIX_process_on_exit_runall(process);

  /* Unregister from the kill timer if any */
  if (process->kill_timer != nullptr)
      SIMIX_timer_remove(process->kill_timer);

  xbt_os_mutex_acquire(simix_global->mutex);

  /* cancel non-blocking communications */
  smx_activity_t synchro = static_cast<smx_activity_t>(process->comms.front());
  while (!process->comms.empty()) {
    simgrid::kernel::activity::Comm *comm = static_cast<simgrid::kernel::activity::Comm*>(synchro);

    /* make sure no one will finish the comm after this process is destroyed,
     * because src_proc or dst_proc would be an invalid pointer */
    comm->cancel();

    if (comm->src_proc == process) {
      XBT_DEBUG("Found an unfinished send comm %p (detached = %d), state %d, src = %p, dst = %p",
          comm, comm->detached, (int)comm->state, comm->src_proc, comm->dst_proc);
      comm->src_proc = nullptr;

      /* I'm not supposed to destroy a detached comm from the sender side, */
      if (comm->detached)
        XBT_DEBUG("Don't destroy it since it's a detached comm and I'm the sender");
      else
        comm->unref();

    }
    else if (comm->dst_proc == process){
      XBT_DEBUG("Found an unfinished recv comm %p, state %d, src = %p, dst = %p",
          comm, (int)comm->state, comm->src_proc, comm->dst_proc);
      comm->dst_proc = nullptr;

      if (comm->detached && comm->src_proc != nullptr) {
        /* the comm will be freed right now, remove it from the sender */
        comm->src_proc->comms.remove(comm);
      }
      
      comm->unref();
    } else {
      xbt_die("Communication synchro %p is in my list but I'm not the sender nor the receiver", synchro);
    }
    process->comms.pop_front();
    synchro = static_cast<smx_activity_t>(process->comms.front());
  }

  XBT_DEBUG("%p should not be run anymore",process);
  simix_global->process_list.erase(process->pid);
  if (process->host)
    xbt_swag_remove(process, process->host->extension<simgrid::simix::Host>()->process_list);
  xbt_swag_insert(process, simix_global->process_to_destroy);
  process->context->iwannadie = 0;

  xbt_os_mutex_release(simix_global->mutex);
}

/**
 * Garbage collection
 *
 * Should be called some time to time to free the memory allocated for processes
 * that have finished (or killed).
 */
void SIMIX_process_empty_trash()
{
  smx_actor_t process = nullptr;

  while ((process = (smx_actor_t) xbt_swag_extract(simix_global->process_to_destroy))) {
    XBT_DEBUG("Getting rid of %p",process);
    intrusive_ptr_release(process);
  }
}

namespace simgrid {
namespace simix {

ActorImpl::~ActorImpl()
{
  delete this->context;
  xbt_dict_free(&this->properties);
}

static int dying_daemon(void* exit_status, void* data)
{
  std::vector<ActorImpl*>* vect = &simix_global->daemons;

  auto it = std::find(vect->begin(), vect->end(), static_cast<ActorImpl*>(data));
  xbt_assert(it != vect->end(), "The dying daemon is not a daemon after all. Please report that bug.");

  /* Don't move the whole content since we don't really care about the order */
  std::swap(*it, vect->back());
  vect->pop_back();

  return 0;
}
/** This process will be terminated automatically when the last non-daemon process finishes */
void ActorImpl::daemonize()
{
  if (!daemon) {
    daemon = true;
    simix_global->daemons.push_back(this);
    SIMIX_process_on_exit(this, dying_daemon, this);
  }
}

/** Whether this process is daemonized */
bool ActorImpl::isDaemon()
{
  return daemon;
}

void create_maestro(std::function<void()> code)
{
  smx_actor_t maestro = nullptr;
  /* Create maestro process and initialize it */
  maestro = new simgrid::simix::ActorImpl();
  maestro->pid = simix_process_maxpid++;
  maestro->name = "";
  maestro->data = nullptr;

  if (!code) {
    maestro->context = SIMIX_context_new(std::function<void()>(), nullptr, maestro);
  } else {
    if (!simix_global)
      xbt_die("simix is not initialized, please call MSG_init first");
    maestro->context =
      simix_global->context_factory->create_maestro(code, maestro);
  }

  maestro->simcall.issuer = maestro;
  simix_global->maestro_process = maestro;
}

}
}

/** @brief Creates and runs the maestro process */
void SIMIX_maestro_create(void (*code)(void*), void* data)
{
  simgrid::simix::create_maestro(std::bind(code, data));
}

/**
 * \brief Internal function to create a process.
 *
 * This function actually creates the process.
 * It may be called when a SIMCALL_PROCESS_CREATE simcall occurs,
 * or directly for SIMIX internal purposes. The sure thing is that it's called from maestro context.
 *
 * \return the process created
 */
smx_actor_t SIMIX_process_create(
                          const char *name,
                          std::function<void()> code,
                          void *data,
                          sg_host_t host,
                          xbt_dict_t properties,
                          smx_actor_t parent_process)
{

  XBT_DEBUG("Start process %s on host '%s'", name, host->cname());

  if (host->isOff()) {
    XBT_WARN("Cannot launch process '%s' on failed host '%s'", name, host->cname());
    return nullptr;
  }

  smx_actor_t process = new simgrid::simix::ActorImpl();

  xbt_assert(code && host != nullptr, "Invalid parameters");
  /* Process data */
  process->pid            = simix_process_maxpid++;
  process->name           = simgrid::xbt::string(name);
  process->host           = host;
  process->data           = data;
  process->simcall.issuer = process;

  if (parent_process != nullptr) {
    process->ppid = parent_process->pid;
/* SMPI process have their own data segment and each other inherit from their father */
#if HAVE_SMPI
    if (smpi_privatize_global_variables == SMPI_PRIVATIZE_MMAP) {
      if (parent_process->pid != 0) {
        SIMIX_segment_index_set(process, parent_process->segment_index);
      } else {
        SIMIX_segment_index_set(process, process->pid - 1);
      }
    }
#endif
  }

  process->code         = code;

  XBT_VERB("Create context %s", process->name.c_str());
  process->context = SIMIX_context_new(std::move(code), simix_global->cleanup_process_function, process);

  /* Add properties */
  process->properties = properties;

  /* Add the process to it's host process list */
  xbt_swag_insert(process, host->extension<simgrid::simix::Host>()->process_list);

  XBT_DEBUG("Start context '%s'", process->name.c_str());

  /* Now insert it in the global process list and in the process to run list */
  simix_global->process_list[process->pid] = process;
  XBT_DEBUG("Inserting %s(%s) in the to_run list", process->cname(), host->cname());
  xbt_dynar_push_as(simix_global->process_to_run, smx_actor_t, process);

  /* Tracing the process creation */
  TRACE_msg_process_create(process->cname(), process->pid, process->host);

  return process;
}

smx_actor_t SIMIX_process_attach(const char* name, void* data, const char* hostname, xbt_dict_t properties,
                                 smx_actor_t parent_process)
{
  // This is mostly a copy/paste from SIMIX_process_new(),
  // it'd be nice to share some code between those two functions.

  sg_host_t host = sg_host_by_name(hostname);
  XBT_DEBUG("Attach process %s on host '%s'", name, hostname);

  if (host->isOff()) {
    XBT_WARN("Cannot launch process '%s' on failed host '%s'",
      name, hostname);
    return nullptr;
  }

  smx_actor_t process = new simgrid::simix::ActorImpl();
  /* Process data */
  process->pid = simix_process_maxpid++;
  process->name = std::string(name);
  process->host = host;
  process->data = data;
  process->simcall.issuer = process;

  if (parent_process != nullptr) {
    process->ppid = parent_process->pid;
    /* SMPI process have their own data segment and each other inherit from their father */
#if HAVE_SMPI
    if (smpi_privatize_global_variables == SMPI_PRIVATIZE_MMAP) {
      if (parent_process->pid != 0) {
        SIMIX_segment_index_set(process, parent_process->segment_index);
      } else {
        SIMIX_segment_index_set(process, process->pid - 1);
      }
    }
#endif
  }

  /* Process data for auto-restart */
  process->code = nullptr;

  XBT_VERB("Create context %s", process->name.c_str());
  if (!simix_global)
    xbt_die("simix is not initialized, please call MSG_init first");
  process->context = simix_global->context_factory->attach(
    simix_global->cleanup_process_function, process);

  /* Add properties */
  process->properties = properties;

  /* Add the process to it's host process list */
  xbt_swag_insert(process, host->extension<simgrid::simix::Host>()->process_list);

  /* Now insert it in the global process list and in the process to run list */
  simix_global->process_list[process->pid] = process;
  XBT_DEBUG("Inserting %s(%s) in the to_run list", process->cname(), host->cname());
  xbt_dynar_push_as(simix_global->process_to_run, smx_actor_t, process);

  /* Tracing the process creation */
  TRACE_msg_process_create(process->cname(), process->pid, process->host);

  auto context = dynamic_cast<simgrid::kernel::context::AttachContext*>(process->context);
  if (!context)
    xbt_die("Not a suitable context");

  context->attach_start();
  return process;
}

void SIMIX_process_detach()
{
  auto context = dynamic_cast<simgrid::kernel::context::AttachContext*>(SIMIX_context_self());
  if (!context)
    xbt_die("Not a suitable context");

  simix_global->cleanup_process_function(context->process());

  // Let maestro ignore we are still alive:
  // xbt_swag_remove(context->process(), simix_global->process_list);

  // TODO, Remove from proces list:
  //   xbt_swag_remove(process, sg_host_simix(host)->process_list);

  context->attach_stop();
  // delete context;
}

/**
 * \brief Executes the processes from simix_global->process_to_run.
 *
 * The processes of simix_global->process_to_run are run (in parallel if
 * possible).  On exit, simix_global->process_to_run is empty, and
 * simix_global->process_that_ran contains the list of processes that just ran.
 * The two lists are swapped so, be careful when using them before and after a
 * call to this function.
 */
void SIMIX_process_runall()
{
  SIMIX_context_runall();

  xbt_dynar_t tmp = simix_global->process_that_ran;
  simix_global->process_that_ran = simix_global->process_to_run;
  simix_global->process_to_run = tmp;
  xbt_dynar_reset(simix_global->process_to_run);
}

void simcall_HANDLER_process_kill(smx_simcall_t simcall, smx_actor_t process) {
  SIMIX_process_kill(process, simcall->issuer);
}
/**
 * \brief Internal function to kill a SIMIX process.
 *
 * This function may be called when a SIMCALL_PROCESS_KILL simcall occurs,
 * or directly for SIMIX internal purposes.
 *
 * \param process poor victim
 * \param issuer the process which has sent the PROCESS_KILL. Important to not schedule twice the same process.
 */
void SIMIX_process_kill(smx_actor_t process, smx_actor_t issuer) {

  XBT_DEBUG("Killing process %s@%s", process->cname(), process->host->cname());

  process->context->iwannadie = 1;
  process->blocked = 0;
  process->suspended = 0;
  process->exception = nullptr;

  /* destroy the blocking synchro if any */
  if (process->waiting_synchro) {

    simgrid::kernel::activity::Exec *exec = dynamic_cast<simgrid::kernel::activity::Exec*>(process->waiting_synchro);
    simgrid::kernel::activity::Comm *comm = dynamic_cast<simgrid::kernel::activity::Comm*>(process->waiting_synchro);
    simgrid::kernel::activity::Sleep *sleep = dynamic_cast<simgrid::kernel::activity::Sleep*>(process->waiting_synchro);
    simgrid::kernel::activity::Raw *raw = dynamic_cast<simgrid::kernel::activity::Raw*>(process->waiting_synchro);
    simgrid::kernel::activity::Io *io = dynamic_cast<simgrid::kernel::activity::Io*>(process->waiting_synchro);

    if (exec != nullptr) {
      exec->unref();

    } else if (comm != nullptr) {
      process->comms.remove(process->waiting_synchro);
      comm->cancel();

      // Remove first occurrence of &process->simcall:
      auto i = boost::range::find(
        process->waiting_synchro->simcalls,
        &process->simcall);
      if (i != process->waiting_synchro->simcalls.end())
        process->waiting_synchro->simcalls.remove(&process->simcall);

      comm->unref();

    } else if (sleep != nullptr) {
      SIMIX_process_sleep_destroy(process->waiting_synchro);

    } else if (raw != nullptr) {
      SIMIX_synchro_stop_waiting(process, &process->simcall);
      delete process->waiting_synchro;

    } else if (io != nullptr) {
      SIMIX_io_destroy(process->waiting_synchro);
    }

    /*
    switch (process->waiting_synchro->type) {
    case SIMIX_SYNC_JOIN:
      SIMIX_process_sleep_destroy(process->waiting_synchro);
      break;
    } */

    process->waiting_synchro = nullptr;
  }
  if(!xbt_dynar_member(simix_global->process_to_run, &(process)) && process != issuer) {
    XBT_DEBUG("Inserting %s in the to_run list", process->name.c_str());
    xbt_dynar_push_as(simix_global->process_to_run, smx_actor_t, process);
  }
}

/** @brief Ask another process to raise the given exception
 *
 * @param process The process that should raise that exception
 * @param cat category of exception
 * @param value value associated to the exception
 * @param msg string information associated to the exception
 */
void SIMIX_process_throw(smx_actor_t process, xbt_errcat_t cat, int value, const char *msg) {
  SMX_EXCEPTION(process, cat, value, msg);

  if (process->suspended)
    SIMIX_process_resume(process);

  /* cancel the blocking synchro if any */
  if (process->waiting_synchro) {

    simgrid::kernel::activity::Exec *exec = dynamic_cast<simgrid::kernel::activity::Exec*>(process->waiting_synchro);
    if (exec != nullptr) {
      SIMIX_execution_cancel(process->waiting_synchro);
    }

    simgrid::kernel::activity::Comm *comm = dynamic_cast<simgrid::kernel::activity::Comm*>(process->waiting_synchro);
    if (comm != nullptr) {
      process->comms.remove(comm);
      comm->cancel();
    }

    simgrid::kernel::activity::Sleep *sleep = dynamic_cast<simgrid::kernel::activity::Sleep*>(process->waiting_synchro);
    if (sleep != nullptr) {
      SIMIX_process_sleep_destroy(process->waiting_synchro);
      if (!xbt_dynar_member(simix_global->process_to_run, &(process)) && process != SIMIX_process_self()) {
        XBT_DEBUG("Inserting %s in the to_run list", process->name.c_str());
        xbt_dynar_push_as(simix_global->process_to_run, smx_actor_t, process);
      }
    }

    simgrid::kernel::activity::Raw *raw = dynamic_cast<simgrid::kernel::activity::Raw*>(process->waiting_synchro);
    if (raw != nullptr) {
      SIMIX_synchro_stop_waiting(process, &process->simcall);
    }

    simgrid::kernel::activity::Io *io = dynamic_cast<simgrid::kernel::activity::Io*>(process->waiting_synchro);
    if (io != nullptr) {
      SIMIX_io_destroy(process->waiting_synchro);
    }
  }
  process->waiting_synchro = nullptr;

}

void simcall_HANDLER_process_killall(smx_simcall_t simcall, int reset_pid) {
  SIMIX_process_killall(simcall->issuer, reset_pid);
}
/**
 * \brief Kills all running processes.
 * \param issuer this one will not be killed
 */
void SIMIX_process_killall(smx_actor_t issuer, int reset_pid)
{
  for (auto kv : simix_global->process_list)
    if (kv.second != issuer)
      SIMIX_process_kill(kv.second, issuer);

  if (reset_pid > 0)
    simix_process_maxpid = reset_pid;

  SIMIX_context_runall();

  SIMIX_process_empty_trash();
}

void simcall_HANDLER_process_set_host(smx_simcall_t simcall, smx_actor_t process, sg_host_t dest)
{
  process->new_host = dest;
}
void SIMIX_process_change_host(smx_actor_t process, sg_host_t dest)
{
  xbt_assert((process != nullptr), "Invalid parameters");
  xbt_swag_remove(process, process->host->extension<simgrid::simix::Host>()->process_list);
  process->host = dest;
  xbt_swag_insert(process, dest->extension<simgrid::simix::Host>()->process_list);
}


void simcall_HANDLER_process_suspend(smx_simcall_t simcall, smx_actor_t process)
{
  smx_activity_t sync_suspend = SIMIX_process_suspend(process, simcall->issuer);

  if (process != simcall->issuer) {
    SIMIX_simcall_answer(simcall);
  } else {
    sync_suspend->simcalls.push_back(simcall);
    process->waiting_synchro = sync_suspend;
    process->waiting_synchro->suspend();
  }
  /* If we are suspending ourselves, then just do not finish the simcall now */
}

smx_activity_t SIMIX_process_suspend(smx_actor_t process, smx_actor_t issuer)
{
  if (process->suspended) {
    XBT_DEBUG("Process '%s' is already suspended", process->name.c_str());
    return nullptr;
  }

  process->suspended = 1;

  /* If we are suspending another process that is waiting on a sync, suspend its synchronization. */
  if (process != issuer) {

    if (process->waiting_synchro)
      process->waiting_synchro->suspend();
    /* If the other process is not waiting, its suspension is delayed to when the process is rescheduled. */

    return nullptr;
  } else {
    return SIMIX_execution_start(process, "suspend", 0.0, 1.0, 0.0);
  }
}

void SIMIX_process_resume(smx_actor_t process)
{
  XBT_IN("process = %p", process);

  if(process->context->iwannadie) {
    XBT_VERB("Ignoring request to suspend a process that is currently dying.");
    return;
  }

  if(!process->suspended) return;
  process->suspended = 0;

  /* resume the synchronization that was blocking the resumed process. */
  if (process->waiting_synchro)
    process->waiting_synchro->resume();

  XBT_OUT();
}

int SIMIX_process_get_maxpid() {
  return simix_process_maxpid;
}

int SIMIX_process_count()
{
  return simix_global->process_list.size();
}

int SIMIX_process_get_PID(smx_actor_t self)
{
  if (self == nullptr)
    return 0;
  else
    return self->pid;
}

void* SIMIX_process_self_get_data()
{
  smx_actor_t self = SIMIX_process_self();

  if (!self) {
    return nullptr;
  }
  return self->data;
}

void SIMIX_process_self_set_data(void *data)
{
  smx_actor_t self = SIMIX_process_self();

  SIMIX_process_set_data(self, data);
}

void SIMIX_process_set_data(smx_actor_t process, void *data)
{
  process->data = data;
}

/* needs to be public and without simcall because it is called
   by exceptions and logging events */
const char* SIMIX_process_self_get_name() {

  smx_actor_t process = SIMIX_process_self();
  if (process == nullptr || process == simix_global->maestro_process)
    return "maestro";

  return process->name.c_str();
}

smx_actor_t SIMIX_process_get_by_name(const char* name)
{
  for (auto kv : simix_global->process_list)
    if (kv.second->name == name)
      return kv.second;
  return nullptr;
}

int SIMIX_process_is_suspended(smx_actor_t process)
{
  return process->suspended;
}

xbt_dict_t SIMIX_process_get_properties(smx_actor_t process)
{
  return process->properties;
}

void simcall_HANDLER_process_join(smx_simcall_t simcall, smx_actor_t process, double timeout)
{
  if (process->finished) {
    // The joined process is already finished, just wake up the issuer process right away
    simcall_process_sleep__set__result(simcall, SIMIX_DONE);
    SIMIX_simcall_answer(simcall);
    return;
  }
  smx_activity_t sync = SIMIX_process_join(simcall->issuer, process, timeout);
  sync->simcalls.push_back(simcall);
  simcall->issuer->waiting_synchro = sync;
}

static int SIMIX_process_join_finish(smx_process_exit_status_t status, smx_activity_t synchro){
  simgrid::kernel::activity::Sleep *sleep = static_cast<simgrid::kernel::activity::Sleep*>(synchro);

  if (sleep->surf_sleep) {
    sleep->surf_sleep->cancel();

    while (!sleep->simcalls.empty()) {
      smx_simcall_t simcall = sleep->simcalls.front();
      sleep->simcalls.pop_front();
      simcall_process_sleep__set__result(simcall, SIMIX_DONE);
      simcall->issuer->waiting_synchro = nullptr;
      if (simcall->issuer->suspended) {
        XBT_DEBUG("Wait! This process is suspended and can't wake up now.");
        simcall->issuer->suspended = 0;
        simcall_HANDLER_process_suspend(simcall, simcall->issuer);
      } else {
        SIMIX_simcall_answer(simcall);
      }
    }
    sleep->surf_sleep->unref();
    sleep->surf_sleep = nullptr;
  }
  sleep->unref();
  // intrusive_ptr_release(process); // FIXME: We are leaking here. See comment in SIMIX_process_join()
  return 0;
}

smx_activity_t SIMIX_process_join(smx_actor_t issuer, smx_actor_t process, double timeout)
{
  smx_activity_t res = SIMIX_process_sleep(issuer, timeout);
  static_cast<simgrid::kernel::activity::ActivityImpl*>(res)->ref();
  /* We are leaking the process here, but if we don't take the ref, we get a "use after free".
   * The correct solution would be to derivate the type SynchroSleep into a SynchroProcessJoin,
   * but the code is not clean enough for now for this.
   * The C API should first be properly replaced with the C++ one, which is a fair amount of work.
   */
  intrusive_ptr_add_ref(process);
  SIMIX_process_on_exit(process, (int_f_pvoid_pvoid_t)SIMIX_process_join_finish, res);
  return res;
}

void simcall_HANDLER_process_sleep(smx_simcall_t simcall, double duration)
{
  if (MC_is_active() || MC_record_replay_is_active()) {
    MC_process_clock_add(simcall->issuer, duration);
    simcall_process_sleep__set__result(simcall, SIMIX_DONE);
    SIMIX_simcall_answer(simcall);
    return;
  }
  smx_activity_t sync = SIMIX_process_sleep(simcall->issuer, duration);
  sync->simcalls.push_back(simcall);
  simcall->issuer->waiting_synchro = sync;
}

smx_activity_t SIMIX_process_sleep(smx_actor_t process, double duration)
{
  sg_host_t host = process->host;

  if (host->isOff())
    THROWF(host_error, 0, "Host %s failed, you cannot sleep there.", host->cname());

  simgrid::kernel::activity::Sleep *synchro = new simgrid::kernel::activity::Sleep();
  synchro->host = host;
  synchro->surf_sleep                       = host->pimpl_cpu->sleep(duration);
  synchro->surf_sleep->setData(synchro);
  XBT_DEBUG("Create sleep synchronization %p", synchro);

  return synchro;
}

void SIMIX_process_sleep_destroy(smx_activity_t synchro)
{
  XBT_DEBUG("Destroy synchro %p", synchro);
  simgrid::kernel::activity::Sleep *sleep = static_cast<simgrid::kernel::activity::Sleep*>(synchro);

  if (sleep->surf_sleep) {
    sleep->surf_sleep->unref();
    sleep->surf_sleep = nullptr;
    sleep->unref();
  }
}

/**
 * \brief Calling this function makes the process to yield.
 *
 * Only the current process can call this function, giving back the control to
 * maestro.
 *
 * \param self the current process
 */
void SIMIX_process_yield(smx_actor_t self)
{
  XBT_DEBUG("Yield actor '%s'", self->cname());

  /* Go into sleep and return control to maestro */
  self->context->suspend();

  /* Ok, maestro returned control to us */
  XBT_DEBUG("Control returned to me: '%s'", self->name.c_str());

  if (self->new_host) {
    SIMIX_process_change_host(self, self->new_host);
    self->new_host = nullptr;
  }

  if (self->context->iwannadie){
    XBT_DEBUG("I wanna die!");
    self->finished = true;
    /* execute the on_exit functions */
    SIMIX_process_on_exit_runall(self);
    /* Add the process to the list of process to restart, only if the host is down */
    if (self->auto_restart && self->host->isOff()) {
      SIMIX_host_add_auto_restart_process(self->host, self->cname(),
                                          self->code, self->data,
                                          SIMIX_timer_get_date(self->kill_timer),
                                          self->properties,
                                          self->auto_restart);
    }
    XBT_DEBUG("Process %s@%s is dead", self->cname(), self->host->cname());
    self->context->stop();
  }

  if (self->suspended) {
    XBT_DEBUG("Hey! I'm suspended.");
    xbt_assert(self->exception != nullptr, "Gasp! This exception may be lost by subsequent calls.");
    self->suspended = 0;
    SIMIX_process_suspend(self, self);
  }

  if (self->exception != nullptr) {
    XBT_DEBUG("Wait, maestro left me an exception");
    std::exception_ptr exception = std::move(self->exception);
    self->exception = nullptr;
    std::rethrow_exception(std::move(exception));
  }

  if(SMPI_switch_data_segment && self->segment_index != -1){
    SMPI_switch_data_segment(self->segment_index);
  }
}

/* callback: termination */
void SIMIX_process_exception_terminate(xbt_ex_t * e)
{
  xbt_ex_display(e);
  xbt_abort();
}

smx_context_t SIMIX_process_get_context(smx_actor_t p) {
  return p->context;
}

void SIMIX_process_set_context(smx_actor_t p,smx_context_t c) {
  p->context = c;
}

/**
 * \brief Returns the list of processes to run.
 */
xbt_dynar_t SIMIX_process_get_runnable()
{
  return simix_global->process_to_run;
}

/**
 * \brief Returns the process from PID.
 */
smx_actor_t SIMIX_process_from_PID(int PID)
{
  if (simix_global->process_list.find(PID) == simix_global->process_list.end())
    return nullptr;
  return simix_global->process_list.at(PID);
}

/** @brief returns a dynar containing all currently existing processes */
xbt_dynar_t SIMIX_processes_as_dynar() {
  xbt_dynar_t res = xbt_dynar_new(sizeof(smx_actor_t),nullptr);
  for (auto kv : simix_global->process_list) {
    smx_actor_t proc = kv.second;
    xbt_dynar_push(res,&proc);
  }
  return res;
}

void SIMIX_process_on_exit_runall(smx_actor_t process) {
  s_smx_process_exit_fun_t exit_fun;
  smx_process_exit_status_t exit_status = (process->context->iwannadie) ? SMX_EXIT_FAILURE : SMX_EXIT_SUCCESS;
  while (!process->on_exit.empty()) {
    exit_fun = process->on_exit.back();
    (exit_fun.fun)((void*)exit_status, exit_fun.arg);
    process->on_exit.pop_back();
  }
}

void SIMIX_process_on_exit(smx_actor_t process, int_f_pvoid_pvoid_t fun, void *data) {
  xbt_assert(process, "current process not found: are you in maestro context ?");

  s_smx_process_exit_fun_t exit_fun = {fun, data};

  process->on_exit.push_back(exit_fun);
}

/**
 * \brief Sets the auto-restart status of the process.
 * If set to 1, the process will be automatically restarted when its host
 * comes back.
 */
void SIMIX_process_auto_restart_set(smx_actor_t process, int auto_restart) {
  process->auto_restart = auto_restart;
}

smx_actor_t simcall_HANDLER_process_restart(smx_simcall_t simcall, smx_actor_t process) {
  return SIMIX_process_restart(process, simcall->issuer);
}
/** @brief Restart a process, starting it again from the beginning. */
smx_actor_t SIMIX_process_restart(smx_actor_t process, smx_actor_t issuer) {
  XBT_DEBUG("Restarting process %s on %s", process->cname(), process->host->cname());

  //retrieve the arguments of the old process
  //FIXME: Factorize this with SIMIX_host_add_auto_restart_process ?
  simgrid::simix::ProcessArg arg;
  arg.name = process->name;
  arg.code = process->code;
  arg.host = process->host;
  arg.kill_time = SIMIX_timer_get_date(process->kill_timer);
  arg.data = process->data;
  arg.properties = nullptr;
  arg.auto_restart = process->auto_restart;

  //kill the old process
  SIMIX_process_kill(process, issuer);

  //start the new process
  smx_actor_t actor = simix_global->create_process_function(arg.name.c_str(), std::move(arg.code), arg.data, arg.host,
                                                            arg.properties, nullptr);
  if (arg.kill_time >= 0)
    simcall_process_set_kill_time(actor, arg.kill_time);
  if (arg.auto_restart)
    simcall_process_auto_restart_set(actor, arg.auto_restart);

  return actor;
}

void SIMIX_segment_index_set(smx_actor_t proc, int index){
  proc->segment_index = index;
}

/**
 * \ingroup simix_process_management
 * \brief Creates and runs a new SIMIX process.
 *
 * The structure and the corresponding thread are created and put in the list of ready processes.
 *
 * \param name a name for the process. It is for user-level information and can be nullptr.
 * \param code the main function of the process
 * \param data a pointer to any data one may want to attach to the new object. It is for user-level information and can be nullptr.
 * It can be retrieved with the function \ref simcall_process_get_data.
 * \param host where the new agent is executed.
 * \param kill_time time when the process is killed
 * \param argc first argument passed to \a code
 * \param argv second argument passed to \a code
 * \param properties the properties of the process
 * \param auto_restart either it is autorestarting or not.
 */
smx_actor_t simcall_process_create(const char* name, xbt_main_func_t code, void* data, sg_host_t host, int argc,
                                   char** argv, xbt_dict_t properties)
{
  if (name == nullptr)
    name = "";
  auto wrapped_code = simgrid::xbt::wrapMain(code, argc, argv);
  for (int i = 0; i != argc; ++i)
    xbt_free(argv[i]);
  xbt_free(argv);
  smx_actor_t res = simcall_process_create(name, std::move(wrapped_code), data, host, properties);
  return res;
}

smx_actor_t simcall_process_create(const char* name, std::function<void()> code, void* data, sg_host_t host,
                                   xbt_dict_t properties)
{
  if (name == nullptr)
    name = "";
  smx_actor_t self = SIMIX_process_self();
  return simgrid::simix::kernelImmediate([name, code, data, host, properties, self] {
    return SIMIX_process_create(name, std::move(code), data, host, properties, self);
  });
}
