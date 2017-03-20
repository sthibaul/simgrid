/* Copyright (c) 2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <cerrno>
#include <cstdlib>

#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <xbt/log.h>
#include <xbt/mmalloc.h>
#include <xbt/swag.h>
#include <xbt/sysdep.h>

#include <simgrid/modelchecker.h>

#include "src/internal_config.h"

#include "src/mc/mc_request.h"
#include "src/mc/remote/Client.hpp"
#include "src/mc/remote/mc_protocol.h"

// We won't need those once the separation MCer/MCed is complete:
#include "src/mc/mc_ignore.h"
#include "src/mc/mc_smx.h"

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_client, mc, "MC client logic");

namespace simgrid {
namespace mc {

std::unique_ptr<Client> Client::client_;

Client* Client::initialize()
{
  // We are not in MC mode:
  // TODO, handle this more gracefully.
  if (!std::getenv(MC_ENV_SOCKET_FD))
    return nullptr;

  // Do not break if we are called multiple times:
  if (client_)
    return client_.get();

  _sg_do_model_check = 1;

  // Fetch socket from MC_ENV_SOCKET_FD:
  char* fd_env = std::getenv(MC_ENV_SOCKET_FD);
  if (!fd_env)
    xbt_die("No MC socket passed in the environment");
  int fd =
      xbt_str_parse_int(fd_env, bprintf("Variable %s should contain a number but contains '%%s'", MC_ENV_SOCKET_FD));
  XBT_DEBUG("Model-checked application found socket FD %i", fd);

  // Check the socket type/validity:
  int type;
  socklen_t socklen = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &socklen) != 0)
    xbt_die("Could not check socket type");
  if (type != SOCK_DGRAM)
    xbt_die("Unexpected socket type %i", type);
  XBT_DEBUG("Model-checked application found expected socket type");

  client_ = std::unique_ptr<Client>(new simgrid::mc::Client(fd));

  // Wait for the model-checker:
  errno = 0;
#if defined __linux__
  ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
#elif defined BSD
  ptrace(PT_TRACE_ME, 0, nullptr, 0);
#else
#error "no ptrace equivalent coded for this platform"
#endif
  if (errno != 0 || raise(SIGSTOP) != 0)
    xbt_die("Could not wait for the model-checker");

  client_->handleMessages();
  return client_.get();
}

void Client::handleMessages()
{
  while (1) {
    XBT_DEBUG("Waiting messages from model-checker");

    char message_buffer[MC_MESSAGE_LENGTH];
    ssize_t received_size;

    if ((received_size = channel_.receive(&message_buffer, sizeof(message_buffer))) < 0)
      xbt_die("Could not receive commands from the model-checker");

    s_mc_message_t message;
    if ((size_t)received_size < sizeof(message))
      xbt_die("Received message is too small");
    memcpy(&message, message_buffer, sizeof(message));
    switch (message.type) {

      case MC_MESSAGE_DEADLOCK_CHECK: {
        // Check deadlock:
        bool deadlock = false;
        if (!simix_global->process_list.empty()) {
          deadlock = true;
          for (auto kv : simix_global->process_list)
            if (simgrid::mc::actor_is_enabled(kv.second)) {
              deadlock = false;
              break;
            }
        }

        // Send result:
        s_mc_int_message_t answer;
        answer.type  = MC_MESSAGE_DEADLOCK_CHECK_REPLY;
        answer.value = deadlock;
        xbt_assert(channel_.send(answer) == 0, "Could not send response");
      } break;

      case MC_MESSAGE_CONTINUE:
        return;

      case MC_MESSAGE_SIMCALL_HANDLE: {
        s_mc_simcall_handle_message_t message;
        if (received_size != sizeof(message))
          xbt_die("Unexpected size for SIMCALL_HANDLE");
        memcpy(&message, message_buffer, sizeof(message));
        smx_actor_t process = SIMIX_process_from_PID(message.pid);
        if (!process)
          xbt_die("Invalid pid %lu", (unsigned long)message.pid);
        SIMIX_simcall_handle(&process->simcall, message.value);
        if (channel_.send(MC_MESSAGE_WAITING))
          xbt_die("Could not send MESSAGE_WAITING to model-checker");
      } break;

      case MC_MESSAGE_RESTORE: {
        s_mc_restore_message_t message;
        if (received_size != sizeof(message))
          xbt_die("Unexpected size for SIMCALL_HANDLE");
        memcpy(&message, message_buffer, sizeof(message));
#if HAVE_SMPI
        smpi_really_switch_data_segment(message.index);
#endif
      } break;

      default:
        xbt_die("Received unexpected message %s (%i)", MC_message_type_name(message.type), message.type);
    }
  }
}

void Client::mainLoop(void)
{
  while (1) {
    simgrid::mc::wait_for_requests();
    xbt_assert(channel_.send(MC_MESSAGE_WAITING) == 0, "Could not send WAITING message to model-checker");
    this->handleMessages();
  }
}

void Client::reportAssertionFailure(const char* description)
{
  if (channel_.send(MC_MESSAGE_ASSERTION_FAILED))
    xbt_die("Could not send assertion to model-checker");
  this->handleMessages();
}

void Client::ignoreMemory(void* addr, std::size_t size)
{
  s_mc_ignore_memory_message_t message;
  message.type = MC_MESSAGE_IGNORE_MEMORY;
  message.addr = (std::uintptr_t)addr;
  message.size = size;
  if (channel_.send(message))
    xbt_die("Could not send IGNORE_MEMORY mesage to model-checker");
}

void Client::ignoreHeap(void* address, std::size_t size)
{
  xbt_mheap_t heap = mmalloc_get_current_heap();

  s_mc_ignore_heap_message_t message;
  message.type    = MC_MESSAGE_IGNORE_HEAP;
  message.address = address;
  message.size    = size;
  message.block   = ((char*)address - (char*)heap->heapbase) / BLOCKSIZE + 1;
  if (heap->heapinfo[message.block].type == 0) {
    message.fragment = -1;
    heap->heapinfo[message.block].busy_block.ignore++;
  } else {
    message.fragment = ((uintptr_t)(ADDR2UINT(address) % (BLOCKSIZE))) >> heap->heapinfo[message.block].type;
    heap->heapinfo[message.block].busy_frag.ignore[message.fragment]++;
  }

  if (channel_.send(message))
    xbt_die("Could not send ignored region to MCer");
}

void Client::unignoreHeap(void* address, std::size_t size)
{
  s_mc_ignore_memory_message_t message;
  message.type = MC_MESSAGE_UNIGNORE_HEAP;
  message.addr = (std::uintptr_t)address;
  message.size = size;
  if (channel_.send(message))
    xbt_die("Could not send IGNORE_HEAP message to model-checker");
}

void Client::declareSymbol(const char* name, int* value)
{
  s_mc_register_symbol_message_t message;
  message.type = MC_MESSAGE_REGISTER_SYMBOL;
  if (strlen(name) + 1 > sizeof(message.name))
    xbt_die("Symbol is too long");
  strncpy(message.name, name, sizeof(message.name));
  message.callback = nullptr;
  message.data     = value;
  if (channel_.send(message))
    xbt_die("Could send REGISTER_SYMBOL message to model-checker");
}

void Client::declareStack(void* stack, size_t size, smx_actor_t process, ucontext_t* context)
{
  xbt_mheap_t heap = mmalloc_get_current_heap();

  s_stack_region_t region;
  memset(&region, 0, sizeof(region));
  region.address = stack;
  region.context = context;
  region.size    = size;
  region.block   = ((char*)stack - (char*)heap->heapbase) / BLOCKSIZE + 1;
#if HAVE_SMPI
  if (smpi_privatize_global_variables == SMPI_PRIVATIZE_MMAP && process)
    region.process_index = smpi_process_index_of_smx_process(process);
  else
#endif
    region.process_index = -1;

  s_mc_stack_region_message_t message;
  message.type         = MC_MESSAGE_STACK_REGION;
  message.stack_region = region;
  if (channel_.send(message))
    xbt_die("Coule not send STACK_REGION to model-checker");
}
}
}
