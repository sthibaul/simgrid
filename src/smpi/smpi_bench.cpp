/* Copyright (c) 2007, 2009-2017. The SimGrid Team. All rights reserved.    */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <cstring>

#include <unordered_map>
#include <utility>

#include "src/internal_config.h"
#include "private.h"
#include "private.hpp"
#include <xbt/ex.hpp>
#include "xbt/dict.h"
#include "xbt/sysdep.h"
#include "xbt/ex.h"
#include "surf/surf.h"
#include "simgrid/sg_config.h"
#include "simgrid/modelchecker.h"
#include "src/mc/mc_replay.h"

#include <sys/types.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h> // sqrt
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#if HAVE_PAPI
#include <papi.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(smpi_bench, smpi, "Logging specific to SMPI (benchmarking)");

/* Shared allocations are handled through shared memory segments.
 * Associated data and metadata are used as follows:
 *
 *                                                                    mmap #1
 *    `allocs' dict                                                     ---- -.
 *    ----------      shared_data_t               shared_metadata_t   / |  |  |
 * .->| <name> | ---> -------------------- <--.   -----------------   | |  |  |
 * |  ----------      | fd of <name>     |    |   | size of mmap  | --| |  |  |
 * |                  | count (2)        |    |-- | data          |   \ |  |  |
 * `----------------- | <name>           |    |   -----------------     ----  |
 *                    --------------------    |   ^                           |
 *                                            |   |                           |
 *                                            |   |   `allocs_metadata' dict  |
 *                                            |   |   ----------------------  |
 *                                            |   `-- | <addr of mmap #1>  |<-'
 *                                            |   .-- | <addr of mmap #2>  |<-.
 *                                            |   |   ----------------------  |
 *                                            |   |                           |
 *                                            |   |                           |
 *                                            |   |                           |
 *                                            |   |                   mmap #2 |
 *                                            |   v                     ---- -'
 *                                            |   shared_metadata_t   / |  |
 *                                            |   -----------------   | |  |
 *                                            |   | size of mmap  | --| |  |
 *                                            `-- | data          |   | |  |
 *                                                -----------------   | |  |
 *                                                                    \ |  |
 *                                                                      ----
 */

#define PTR_STRLEN (2 + 2 * sizeof(void*) + 1)

xbt_dict_t samples = nullptr;         /* Allocated on first use */
xbt_dict_t calls = nullptr;           /* Allocated on first use */

double smpi_cpu_threshold;
double smpi_host_speed;

int smpi_loaded_page = -1;
char* smpi_start_data_exe = nullptr;
int smpi_size_data_exe = 0;
int smpi_privatize_global_variables;
shared_malloc_type smpi_cfg_shared_malloc = shmalloc_global;
double smpi_total_benched_time = 0;
smpi_privatisation_region_t smpi_privatisation_regions;

namespace {

/** Some location in the source code
 *
 *  This information is used by SMPI_SHARED_MALLOC to allocate  some shared memory for all simulated processes.
 */
class smpi_source_location {
public:
  smpi_source_location(const char* filename, int line)
      : filename(xbt_strdup(filename)), filename_length(strlen(filename)), line(line)
  {
  }

  /** Pointer to a static string containing the file name */
  char* filename      = nullptr;
  int filename_length = 0;
  int line            = 0;

  bool operator==(smpi_source_location const& that) const
  {
    return filename_length == that.filename_length && line == that.line &&
           std::memcmp(filename, that.filename, filename_length) == 0;
  }
  bool operator!=(smpi_source_location const& that) const { return !(*this == that); }
};
}

namespace std {

template <> class hash<smpi_source_location> {
public:
  typedef smpi_source_location argument_type;
  typedef std::size_t result_type;
  result_type operator()(smpi_source_location const& loc) const
  {
    return xbt_str_hash_ext(loc.filename, loc.filename_length) ^
           xbt_str_hash_ext((const char*)&loc.line, sizeof(loc.line));
  }
};
}

namespace {

typedef struct {
  int fd    = -1;
  int count = 0;
} shared_data_t;

std::unordered_map<smpi_source_location, shared_data_t> allocs;
typedef std::unordered_map<smpi_source_location, shared_data_t>::value_type shared_data_key_type;

typedef struct {
  size_t size;
  shared_data_key_type* data;
} shared_metadata_t;

std::unordered_map<void*, shared_metadata_t> allocs_metadata;
}

static size_t shm_size(int fd) {
  struct stat st;

  if(fstat(fd, &st) < 0) {
    xbt_die("Could not stat fd %d: %s", fd, strerror(errno));
  }
  return static_cast<size_t>(st.st_size);
}

#ifndef WIN32
static void* shm_map(int fd, size_t size, shared_data_key_type* data) {
  char loc[PTR_STRLEN];
  shared_metadata_t meta;

  if(size > shm_size(fd) && (ftruncate(fd, static_cast<off_t>(size)) < 0)) {
    xbt_die("Could not truncate fd %d to %zu: %s", fd, size, strerror(errno));
  }

  void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(mem == MAP_FAILED) {
    xbt_die(
        "Failed to map fd %d with size %zu: %s\n"
        "If you are running a lot of ranks, you may be exceeding the amount of mappings allowed per process.\n"
        "On Linux systems, change this value with sudo sysctl -w vm.max_map_count=newvalue (default value: 65536)\n"
        "Please see http://simgrid.gforge.inria.fr/simgrid/latest/doc/html/options.html#options_virt for more info.",
        fd, size, strerror(errno));
  }
  snprintf(loc, PTR_STRLEN, "%p", mem);
  meta.size = size;
  meta.data = data;
  allocs_metadata[mem] = meta;
  XBT_DEBUG("MMAP %zu to %p", size, mem);
  return mem;
}
#endif

void smpi_bench_destroy()
{
  allocs.clear();
  allocs_metadata.clear();
  xbt_dict_free(&samples);
  xbt_dict_free(&calls);
}

extern "C" XBT_PUBLIC(void) smpi_execute_flops_(double *flops);
void smpi_execute_flops_(double *flops)
{
  smpi_execute_flops(*flops);
}

extern "C" XBT_PUBLIC(void) smpi_execute_(double *duration);
void smpi_execute_(double *duration)
{
  smpi_execute(*duration);
}

void smpi_execute_flops(double flops) {
  XBT_DEBUG("Handle real computation time: %f flops", flops);
  smx_activity_t action = simcall_execution_start("computation", flops, 1, 0);
  simcall_set_category (action, TRACE_internal_smpi_get_category());
  simcall_execution_wait(action);
  smpi_switch_data_segment(smpi_process_index());
}

void smpi_execute(double duration)
{
  if (duration >= smpi_cpu_threshold) {
    XBT_DEBUG("Sleep for %g to handle real computation time", duration);
    double flops = duration * smpi_host_speed;
    int rank = smpi_process_index();
    instr_extra_data extra = xbt_new0(s_instr_extra_data_t,1);
    extra->type=TRACING_COMPUTING;
    extra->comp_size=flops;
    TRACE_smpi_computing_in(rank, extra);

    smpi_execute_flops(flops);

    TRACE_smpi_computing_out(rank);

  } else {
    XBT_DEBUG("Real computation took %g while option smpi/cpu_threshold is set to %g => ignore it", duration,
              smpi_cpu_threshold);
  }
}

void smpi_bench_begin()
{
  if (smpi_privatize_global_variables == SMPI_PRIVATIZE_MMAP) {
    smpi_switch_data_segment(smpi_process_index());
  }

  if (MC_is_active() || MC_record_replay_is_active())
    return;

#if HAVE_PAPI
  if (xbt_cfg_get_string("smpi/papi-events")[0] != '\0') {
    int event_set = smpi_process_papi_event_set();
    // PAPI_start sets everything to 0! See man(3) PAPI_start
    if (PAPI_LOW_LEVEL_INITED == PAPI_is_initialized()) {
      if (PAPI_start(event_set) != PAPI_OK) {
        // TODO This needs some proper handling.
        XBT_CRITICAL("Could not start PAPI counters.\n");
        xbt_die("Error.");
      }
    }
  }
#endif
  xbt_os_threadtimer_start(smpi_process_timer());
}

void smpi_bench_end()
{
  if (MC_is_active() || MC_record_replay_is_active())
    return;

  double speedup = 1;
  xbt_os_timer_t timer = smpi_process_timer();
  xbt_os_threadtimer_stop(timer);

#if HAVE_PAPI
  /**
   * An MPI function has been called and now is the right time to update
   * our PAPI counters for this process.
   */
  if (xbt_cfg_get_string("smpi/papi-events")[0] != '\0') {
    papi_counter_t& counter_data        = smpi_process_papi_counters();
    int event_set                       = smpi_process_papi_event_set();
    std::vector<long long> event_values = std::vector<long long>(counter_data.size());

    if (PAPI_stop(event_set, &event_values[0]) != PAPI_OK) { // Error
      XBT_CRITICAL("Could not stop PAPI counters.\n");
      xbt_die("Error.");
    } else {
      for (unsigned int i = 0; i < counter_data.size(); i++) {
        counter_data[i].second += event_values[i];
        // XBT_DEBUG("[%i] PAPI: Counter %s: Value is now %lli (got increment by %lli\n", smpi_process_index(),
        // counter_data[i].first.c_str(), counter_data[i].second, event_values[i]);
      }
    }
  }
#endif

  if (smpi_process_get_sampling()) {
    XBT_CRITICAL("Cannot do recursive benchmarks.");
    XBT_CRITICAL("Are you trying to make a call to MPI within a SMPI_SAMPLE_ block?");
    xbt_backtrace_display_current();
    xbt_die("Aborting.");
  }

  if (xbt_cfg_get_string("smpi/comp-adjustment-file")[0] != '\0') { // Maybe we need to artificially speed up or slow
    // down our computation based on our statistical analysis.

    smpi_trace_call_location_t* loc                            = smpi_process_get_call_location();
    std::string key                                            = loc->get_composed_key();
    std::unordered_map<std::string, double>::const_iterator it = location2speedup.find(key);
    if (it != location2speedup.end()) {
      speedup = it->second;
    }
  }

  // Simulate the benchmarked computation unless disabled via command-line argument
  if (xbt_cfg_get_boolean("smpi/simulate-computation")) {
    smpi_execute(xbt_os_timer_elapsed(timer)/speedup);
  }

#if HAVE_PAPI
  if (xbt_cfg_get_string("smpi/papi-events")[0] != '\0' && TRACE_smpi_is_enabled()) {
    char container_name[INSTR_DEFAULT_STR_SIZE];
    smpi_container(smpi_process_index(), container_name, INSTR_DEFAULT_STR_SIZE);
    container_t container        = PJ_container_get(container_name);
    papi_counter_t& counter_data = smpi_process_papi_counters();

    for (auto& pair : counter_data) {
      new_pajeSetVariable(surf_get_clock(), container,
                          PJ_type_get(/* countername */ pair.first.c_str(), container->type), pair.second);
    }
  }
#endif

  smpi_total_benched_time += xbt_os_timer_elapsed(timer);
}

/* Private sleep function used by smpi_sleep() and smpi_usleep() */
static unsigned int private_sleep(double secs)
{
  smpi_bench_end();

  XBT_DEBUG("Sleep for: %lf secs", secs);
  int rank = MPI_COMM_WORLD->rank();
  instr_extra_data extra = xbt_new0(s_instr_extra_data_t,1);
  extra->type=TRACING_SLEEPING;
  extra->sleep_duration=secs;
  TRACE_smpi_sleeping_in(rank, extra);

  simcall_process_sleep(secs);

  TRACE_smpi_sleeping_out(rank);

  smpi_bench_begin();
  return 0;
}

unsigned int smpi_sleep(unsigned int secs)
{
  return private_sleep(static_cast<double>(secs));
}

int smpi_usleep(useconds_t usecs)
{
  return static_cast<int>(private_sleep(static_cast<double>(usecs) / 1000000.0));
}

#if _POSIX_TIMERS > 0
int smpi_nanosleep(const struct timespec *tp, struct timespec * t)
{
  return static_cast<int>(private_sleep(static_cast<double>(tp->tv_sec + tp->tv_nsec / 1000000000.0)));
}
#endif

int smpi_gettimeofday(struct timeval *tv, void* tz)
{
  smpi_bench_end();
  double now = SIMIX_get_clock();
  if (tv) {
    tv->tv_sec = static_cast<time_t>(now);
#ifdef WIN32
    tv->tv_usec = static_cast<useconds_t>((now - tv->tv_sec) * 1e6);
#else
    tv->tv_usec = static_cast<suseconds_t>((now - tv->tv_sec) * 1e6);
#endif
  }
  smpi_bench_begin();
  return 0;
}

#if _POSIX_TIMERS > 0
int smpi_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
  //there is only one time in SMPI, so clk_id is ignored.
  smpi_bench_end();
  double now = SIMIX_get_clock();
  if (tp) {
    tp->tv_sec = static_cast<time_t>(now);
    tp->tv_nsec = static_cast<long int>((now - tp->tv_sec) * 1e9);
  }
  smpi_bench_begin();
  return 0;
}
#endif

extern double sg_surf_precision;
unsigned long long smpi_rastro_resolution ()
{
  smpi_bench_end();
  double resolution = (1/sg_surf_precision);
  smpi_bench_begin();
  return static_cast<unsigned long long>(resolution);
}

unsigned long long smpi_rastro_timestamp ()
{
  smpi_bench_end();
  double now = SIMIX_get_clock();

  unsigned long long sec = (unsigned long long)now;
  unsigned long long pre = (now - sec) * smpi_rastro_resolution();
  smpi_bench_begin();
  return static_cast<unsigned long long>(sec) * smpi_rastro_resolution() + pre;
}

/* ****************************** Functions related to the SMPI_SAMPLE_ macros ************************************/
typedef struct {
  double threshold; /* maximal stderr requested (if positive) */
  double relstderr; /* observed stderr so far */
  double mean;      /* mean of benched times, to be used if the block is disabled */
  double sum;       /* sum of benched times (to compute the mean and stderr) */
  double sum_pow2;  /* sum of the square of the benched times (to compute the stderr) */
  int iters;        /* amount of requested iterations */
  int count;        /* amount of iterations done so far */
  int benching;     /* 1: we are benchmarking; 0: we have enough data, no bench anymore */
} local_data_t;

static char *sample_location(int global, const char *file, int line) {
  if (global) {
    return bprintf("%s:%d", file, line);
  } else {
    return bprintf("%s:%d:%d", file, line, smpi_process_index());
  }
}

static int sample_enough_benchs(local_data_t *data) {
  int res = data->count >= data->iters;
  if (data->threshold>0.0) {
    if (data->count <2)
      res = 0; // not enough data
    if (data->relstderr > data->threshold)
      res = 0; // stderr too high yet
  }
  XBT_DEBUG("%s (count:%d iter:%d stderr:%f thres:%f mean:%fs)",
      (res?"enough benchs":"need more data"), data->count, data->iters, data->relstderr, data->threshold, data->mean);
  return res;
}

void smpi_sample_1(int global, const char *file, int line, int iters, double threshold)
{
  char *loc = sample_location(global, file, line);

  smpi_bench_end();     /* Take time from previous, unrelated computation into account */
  smpi_process_set_sampling(1);

  if (samples==nullptr)
    samples = xbt_dict_new_homogeneous(free);

  local_data_t *data = static_cast<local_data_t *>(xbt_dict_get_or_null(samples, loc));
  if (data==nullptr) {
    xbt_assert(threshold>0 || iters>0,
        "You should provide either a positive amount of iterations to bench, or a positive maximal stderr (or both)");
    data = static_cast<local_data_t *>( xbt_new(local_data_t, 1));
    data->count = 0;
    data->sum = 0.0;
    data->sum_pow2 = 0.0;
    data->iters = iters;
    data->threshold = threshold;
    data->benching = 1; // If we have no data, we need at least one
    data->mean = 0;
    xbt_dict_set(samples, loc, data, nullptr);
    XBT_DEBUG("XXXXX First time ever on benched nest %s.",loc);
  } else {
    if (data->iters != iters || data->threshold != threshold) {
      XBT_ERROR("Asked to bench block %s with different settings %d, %f is not %d, %f. "
                "How did you manage to give two numbers at the same line??",
                loc, data->iters, data->threshold, iters, threshold);
      THROW_IMPOSSIBLE;
    }

    // if we already have some data, check whether sample_2 should get one more bench or whether it should emulate
    // the computation instead
    data->benching = (sample_enough_benchs(data) == 0);
    XBT_DEBUG("XXXX Re-entering the benched nest %s. %s", loc,
              (data->benching ? "more benching needed" : "we have enough data, skip computes"));
  }
  xbt_free(loc);
}

int smpi_sample_2(int global, const char *file, int line)
{
  char *loc = sample_location(global, file, line);
  int res;

  xbt_assert(samples, "Y U NO use SMPI_SAMPLE_* macros? Stop messing directly with smpi_sample_* functions!");
  local_data_t *data = static_cast<local_data_t *>(xbt_dict_get(samples, loc));
  XBT_DEBUG("sample2 %s",loc);
  xbt_free(loc);

  if (data->benching==1) {
    // we need to run a new bench
    XBT_DEBUG("benchmarking: count:%d iter:%d stderr:%f thres:%f; mean:%f",
        data->count, data->iters, data->relstderr, data->threshold, data->mean);
    res = 1;
  } else {
    // Enough data, no more bench (either we got enough data from previous visits to this benched nest, or we just
    //ran one bench and need to bail out now that our job is done). Just sleep instead
    XBT_DEBUG("No benchmark (either no need, or just ran one): count >= iter (%d >= %d) or stderr<thres (%f<=%f)."
              " apply the %fs delay instead",
              data->count, data->iters, data->relstderr, data->threshold, data->mean);
    smpi_execute(data->mean);
    smpi_process_set_sampling(0);
    res = 0; // prepare to capture future, unrelated computations
  }
  smpi_bench_begin();
  return res;
}

void smpi_sample_3(int global, const char *file, int line)
{
  char *loc = sample_location(global, file, line);

  xbt_assert(samples, "Y U NO use SMPI_SAMPLE_* macros? Stop messing directly with smpi_sample_* functions!");
  local_data_t *data = static_cast<local_data_t *>(xbt_dict_get(samples, loc));
  XBT_DEBUG("sample3 %s",loc);
  xbt_free(loc);

  if (data->benching==0)
    THROW_IMPOSSIBLE;

  // ok, benchmarking this loop is over
  xbt_os_threadtimer_stop(smpi_process_timer());

  // update the stats
  data->count++;
  double sample = xbt_os_timer_elapsed(smpi_process_timer());
  data->sum += sample;
  data->sum_pow2 += sample * sample;
  double n = static_cast<double>(data->count);
  data->mean = data->sum / n;
  data->relstderr = sqrt((data->sum_pow2 / n - data->mean * data->mean) / n) / data->mean;
  if (sample_enough_benchs(data)==0) {
    data->mean = sample; // Still in benching process; We want sample_2 to simulate the exact time of this loop
    // occurrence before leaving, not the mean over the history
  }
  XBT_DEBUG("Average mean after %d steps is %f, relative standard error is %f (sample was %f)", data->count,
      data->mean, data->relstderr, sample);

  // That's enough for now, prevent sample_2 to run the same code over and over
  data->benching = 0;
}

#ifndef WIN32
static int smpi_shared_malloc_bogusfile           = -1;
static unsigned long smpi_shared_malloc_blocksize = 1UL << 20;
void *smpi_shared_malloc(size_t size, const char *file, int line)
{
  void* mem;
  if (size > 0 && smpi_cfg_shared_malloc == shmalloc_local) {
    smpi_source_location loc(file, line);
    auto res = allocs.insert(std::make_pair(loc, shared_data_t()));
    auto data = res.first;
    if (res.second) {
      // The insertion did not take place.
      // Generate a shared memory name from the address of the shared_data:
      char shmname[32]; // cannot be longer than PSHMNAMLEN = 31 on Mac OS X (shm_open raises ENAMETOOLONG otherwise)
      snprintf(shmname, 31, "/shmalloc%p", &*data);
      int fd = shm_open(shmname, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
      if (fd < 0) {
        if (errno == EEXIST)
          xbt_die("Please cleanup /dev/shm/%s", shmname);
        else
          xbt_die("An unhandled error occurred while opening %s. shm_open: %s", shmname, strerror(errno));
      }
      data->second.fd = fd;
      data->second.count = 1;
      mem = shm_map(fd, size, &*data);
      if (shm_unlink(shmname) < 0) {
        XBT_WARN("Could not early unlink %s. shm_unlink: %s", shmname, strerror(errno));
      }
      XBT_DEBUG("Mapping %s at %p through %d", shmname, mem, fd);
    } else {
      mem = shm_map(data->second.fd, size, &*data);
      data->second.count++;
    }
    XBT_DEBUG("Shared malloc %zu in %p (metadata at %p)", size, mem, &*data);

  } else if (smpi_cfg_shared_malloc == shmalloc_global) {
    /* First reserve memory area */
    mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    xbt_assert(mem != MAP_FAILED, "Failed to allocate %luMiB of memory. Run \"sysctl vm.overcommit_memory=1\" as root "
                                  "to allow big allocations.\n",
               (unsigned long)(size >> 20));

    /* Create bogus file if not done already */
    if (smpi_shared_malloc_bogusfile == -1) {
      /* Create a fd to a new file on disk, make it smpi_shared_malloc_blocksize big, and unlink it.
       * It still exists in memory but not in the file system (thus it cannot be leaked). */
      char* name                   = xbt_strdup("/tmp/simgrid-shmalloc-XXXXXX");
      smpi_shared_malloc_bogusfile = mkstemp(name);
      unlink(name);
      free(name);
      char* dumb = (char*)calloc(1, smpi_shared_malloc_blocksize);
      ssize_t err = write(smpi_shared_malloc_bogusfile, dumb, smpi_shared_malloc_blocksize);
      if(err<0)
        xbt_die("Could not write bogus file for shared malloc");
      free(dumb);
    }

    /* Map the bogus file in place of the anonymous memory */
    unsigned int i;
    for (i = 0; i < size / smpi_shared_malloc_blocksize; i++) {
      void* pos = (void*)((unsigned long)mem + i * smpi_shared_malloc_blocksize);
      void* res = mmap(pos, smpi_shared_malloc_blocksize, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED | MAP_POPULATE,
                       smpi_shared_malloc_bogusfile, 0);
      xbt_assert(res == pos, "Could not map folded virtual memory (%s). Do you perhaps need to increase the "
                             "STARPU_MALLOC_SIMULATION_FOLD environment variable or the sysctl vm.max_map_count?",
                 strerror(errno));
    }
    if (size % smpi_shared_malloc_blocksize) {
      void* pos = (void*)((unsigned long)mem + i * smpi_shared_malloc_blocksize);
      void* res = mmap(pos, size % smpi_shared_malloc_blocksize, PROT_READ | PROT_WRITE,
                       MAP_FIXED | MAP_SHARED | MAP_POPULATE, smpi_shared_malloc_bogusfile, 0);
      xbt_assert(res == pos, "Could not map folded virtual memory (%s). Do you perhaps need to increase the "
                             "STARPU_MALLOC_SIMULATION_FOLD environment variable or the sysctl vm.max_map_count?",
                 strerror(errno));
    }

  } else {
    mem = xbt_malloc(size);
    XBT_DEBUG("Classic malloc %zu in %p", size, mem);
  }

  return mem;
}

void smpi_shared_free(void *ptr)
{
  if (smpi_cfg_shared_malloc == shmalloc_local) {
    char loc[PTR_STRLEN];
    snprintf(loc, PTR_STRLEN, "%p", ptr);
    auto meta = allocs_metadata.find(ptr);
    if (meta == allocs_metadata.end()) {
      XBT_WARN("Cannot free: %p was not shared-allocated by SMPI - maybe its size was 0?", ptr);
      return;
    }
    shared_data_t* data = &meta->second.data->second;
    if (munmap(ptr, meta->second.size) < 0) {
      XBT_WARN("Unmapping of fd %d failed: %s", data->fd, strerror(errno));
    }
    data->count--;
    if (data->count <= 0) {
      close(data->fd);
      allocs.erase(allocs.find(meta->second.data->first));
      XBT_DEBUG("Shared free - with removal - of %p", ptr);
    } else {
      XBT_DEBUG("Shared free - no removal - of %p, count = %d", ptr, data->count);
    }

  } else if (smpi_cfg_shared_malloc == shmalloc_global) {
    munmap(ptr, 0); // the POSIX says that I should not give 0 as a length, but it seems to work OK

  } else {
    XBT_DEBUG("Classic free of %p", ptr);
    xbt_free(ptr);
  }
}
#endif

int smpi_shared_known_call(const char* func, const char* input)
{
  char* loc = bprintf("%s:%s", func, input);
  int known = 0;

  if (calls==nullptr) {
    calls = xbt_dict_new_homogeneous(nullptr);
  }
  try {
    xbt_dict_get(calls, loc); /* Succeed or throw */
    known = 1;
    xbt_free(loc);
  }
  catch (xbt_ex& ex) {
    xbt_free(loc);
    if (ex.category != not_found_error)
      throw;
  }
  catch(...) {
    xbt_free(loc);
    throw;
  }
  return known;
}

void* smpi_shared_get_call(const char* func, const char* input) {
  char* loc = bprintf("%s:%s", func, input);

  if (calls == nullptr)
    calls    = xbt_dict_new_homogeneous(nullptr);
  void* data = xbt_dict_get(calls, loc);
  xbt_free(loc);
  return data;
}

void* smpi_shared_set_call(const char* func, const char* input, void* data) {
  char* loc = bprintf("%s:%s", func, input);

  if (calls == nullptr)
    calls = xbt_dict_new_homogeneous(nullptr);
  xbt_dict_set(calls, loc, data, nullptr);
  xbt_free(loc);
  return data;
}


/** Map a given SMPI privatization segment (make a SMPI process active) */
void smpi_switch_data_segment(int dest) {
  if (smpi_loaded_page == dest)//no need to switch, we've already loaded the one we want
    return;

  // So the job:
  smpi_really_switch_data_segment(dest);
}

/** Map a given SMPI privatization segment (make a SMPI process active)  even if SMPI thinks it is already active
 *
 *  When doing a state restoration, the state of the restored variables  might not be consistent with the state of the
 *  virtual memory. In this case, we to change the data segment.
 */
void smpi_really_switch_data_segment(int dest)
{
  if(smpi_size_data_exe == 0)//no need to switch
    return;

#if HAVE_PRIVATIZATION
  if(smpi_loaded_page==-1){//initial switch, do the copy from the real page here
    for (int i=0; i< smpi_process_count(); i++){
      memcpy(smpi_privatisation_regions[i].address, TOPAGE(smpi_start_data_exe), smpi_size_data_exe);
    }
  }

  // FIXME, cross-process support (mmap across process when necessary)
  int current = smpi_privatisation_regions[dest].file_descriptor;
  XBT_DEBUG("Switching data frame to the one of process %d", dest);
  void* tmp =
      mmap(TOPAGE(smpi_start_data_exe), smpi_size_data_exe, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, current, 0);
  if (tmp != TOPAGE(smpi_start_data_exe))
    xbt_die("Couldn't map the new region");
  smpi_loaded_page = dest;
#endif
}

int smpi_is_privatisation_file(char* file)
{
  return strncmp("/dev/shm/my-buffer-", file, std::strlen("/dev/shm/my-buffer-")) == 0;
}

void smpi_initialize_global_memory_segments()
{

#if !HAVE_PRIVATIZATION
  smpi_privatize_global_variables = SMPI_PRIVATIZE_NONE;
  xbt_die("You are trying to use privatization on a system that does not support it. Please don't.");
  return;
#else

  smpi_get_executable_global_size();

  XBT_DEBUG ("bss+data segment found : size %d starting at %p", smpi_size_data_exe, smpi_start_data_exe );

  if (smpi_size_data_exe == 0){//no need to switch
    smpi_privatize_global_variables = SMPI_PRIVATIZE_NONE;
    return;
  }

  smpi_privatisation_regions = static_cast<smpi_privatisation_region_t>(
      xbt_malloc(smpi_process_count() * sizeof(struct s_smpi_privatisation_region)));

  for (int i=0; i< smpi_process_count(); i++){
    // create SIMIX_process_count() mappings of this size with the same data inside
    int file_descriptor;
    void* address = nullptr;
    char path[24];
    int status;

    do {
      snprintf(path, sizeof(path), "/smpi-buffer-%06x", rand() % 0xffffff);
      file_descriptor = shm_open(path, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    } while (file_descriptor == -1 && errno == EEXIST);
    if (file_descriptor < 0) {
      if (errno == EMFILE) {
        xbt_die("Impossible to create temporary file for memory mapping: %s\n\
The open() system call failed with the EMFILE error code (too many files). \n\n\
This means that you reached the system limits concerning the amount of files per process. \
This is not a surprise if you are trying to virtualize many processes on top of SMPI. \
Don't panic -- you should simply increase your system limits and try again. \n\n\
First, check what your limits are:\n\
  cat /proc/sys/fs/file-max # Gives you the system-wide limit\n\
  ulimit -Hn                # Gives you the per process hard limit\n\
  ulimit -Sn                # Gives you the per process soft limit\n\
  cat /proc/self/limits     # Displays any per-process limitation (including the one given above)\n\n\
If one of these values is less than the amount of MPI processes that you try to run, then you got the explanation of this error. \
Ask the Internet about tutorials on how to increase the files limit such as: https://rtcamp.com/tutorials/linux/increase-open-files-limit/",
                strerror(errno));
      }
      xbt_die("Impossible to create temporary file for memory mapping: %s", strerror(errno));
    }

    status = ftruncate(file_descriptor, smpi_size_data_exe);
    if (status)
      xbt_die("Impossible to set the size of the temporary file for memory mapping");

    /* Ask for a free region */
    address = mmap(nullptr, smpi_size_data_exe, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
    if (address == MAP_FAILED)
      xbt_die("Couldn't find a free region for memory mapping");

    status = shm_unlink(path);
    if (status)
      xbt_die("Impossible to unlink temporary file for memory mapping");

    // initialize the values
    memcpy(address, TOPAGE(smpi_start_data_exe), smpi_size_data_exe);

    // store the address of the mapping for further switches
    smpi_privatisation_regions[i].file_descriptor = file_descriptor;
    smpi_privatisation_regions[i].address         = address;
  }
#endif
}

void smpi_destroy_global_memory_segments(){
  if (smpi_size_data_exe == 0)//no need to switch
    return;
#if HAVE_PRIVATIZATION
  for (int i=0; i< smpi_process_count(); i++) {
    if (munmap(smpi_privatisation_regions[i].address, smpi_size_data_exe) < 0)
      XBT_WARN("Unmapping of fd %d failed: %s", smpi_privatisation_regions[i].file_descriptor, strerror(errno));
    close(smpi_privatisation_regions[i].file_descriptor);
  }
  xbt_free(smpi_privatisation_regions);
#endif
}

extern "C" { /** These functions will be called from the user code **/
  smpi_trace_call_location_t* smpi_trace_get_call_location() {
    return smpi_process_get_call_location();
  }

  void smpi_trace_set_call_location(const char* file, const int line) {
    smpi_trace_call_location_t* loc = smpi_process_get_call_location();

    loc->previous_filename   = loc->filename;
    loc->previous_linenumber = loc->linenumber;
    loc->filename            = file;
    loc->linenumber          = line;
  }

  /**
   * Required for Fortran bindings
   */
  void smpi_trace_set_call_location_(const char* file, int* line) {
    smpi_trace_set_call_location(file, *line);
  }

  /** 
   * Required for Fortran if -fsecond-underscore is activated
   */
  void smpi_trace_set_call_location__(const char* file, int* line) {
    smpi_trace_set_call_location(file, *line);
  }
}
