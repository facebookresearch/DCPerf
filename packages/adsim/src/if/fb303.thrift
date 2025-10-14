/*
 * fb303.thrift
 *
 * Copyright (c) 2006- Facebook
 * Distributed under the Thrift Software License
 *
 * See accompanying file LICENSE or visit the Thrift site at:
 * http://developers.facebook.com/thrift/
 *
 *
 * Definition of common Facebook data types and status reporting mechanisms
 * common to all Facebook services. In some cases, these methods are
 * provided in the base implementation, and in other cases they simply define
 * methods that inheriting applications should implement (i.e. status report)
 *
 * @author Mark Slee <mcslee@facebook.com>
 */
include "fb303/thrift/fb303_core.thrift"
include "thrift/annotation/cpp.thrift"
include "thrift/annotation/thrift.thrift"

namespace java com.facebook.fbcode.fb303
namespace java.swift com.facebook.swift.fb303
namespace java2 com.facebook.thrift.fb303
namespace cpp facebook.fb303
namespace py.asyncio fb303_asyncio.fb303
namespace perl fb303
namespace hack fb303
namespace node_module fb303
namespace go common.fb303.if.fb303

/**
 * For migration to open source fb303_core.
 */
typedef fb303_core.fb303_status fb_status
/*
const fb_status DEAD = fb303_core.DEAD
const fb_status STARTING = fb303_core.STARTING
const fb_status ALIVE = fb303_core.ALIVE
const fb_status STOPPING = fb303_core.STOPPING
const fb_status STOPPED = fb303_core.STOPPED
const fb_status WARNING = fb303_core.WARNING
*/

/**
 * DEPRECATED! This will be removed soon.
 *
 * Structure for holding counters information.
 */
struct CountersInformation {
  1: map<string, i64> data;
}

struct CpuProfileOptions {
  1: i32 durationSecs;

  // If "selective" is set, we only profile sections of code where profiling
  // is explicitly enabled by calling Perftools::startSelectiveCpuProfiling()
  // and Perftools::stopSelectiveCpuProfiling() from each thread that
  // should be profiled. You must likely instrument your server to do this.
  2: bool selective;
}

/**
 * Standard base service
 *
 * Those methods with priority level IMPORTANT run in a dedicated thread pool
 * in C++. If you need to change one of the rest to be IMPORTANT, make sure it
 * runs fast so it won't block other IMPORTANT methods.
 */
service FacebookService extends fb303_core.BaseService {
  /**
   * Returns a CPU profile over the given time interval (client and server
   * must agree on the profile format).
   *
   */
  @cpp.ProcessInEbThreadUnsafe
  string getCpuProfile(1: i32 profileDurationInSec);

  /**
   * Returns a CPU profile, specifying options (see the struct definition).
   */
  @cpp.ProcessInEbThreadUnsafe
  string getCpuProfileWithOptions(1: CpuProfileOptions options);

  /**
   * Returns a WallTime Profiler data over the given time
   * interval (client and server must agree on the profile format).
   */
  string getWallTimeProfile(1: i32 profileDurationInSec);

  /**
   * Returns the current memory usage (RSS) of this process in bytes.
   */
  i64 getMemoryUsage();

  /**
   * Returns the 1-minute load average on the system (i.e. the load may not
   * all be coming from the current process).
   */
  @thrift.Priority{level = thrift.RpcPriority.IMPORTANT}
  double getLoad();

  /**
   * Returns the pid of the process
   */
  i64 getPid();

  /**
   * Returns the command line used to execute this process.
   */
  string getCommandLine();

  /**
   * Tell the server to reload its configuration, reopen log files, etc
   */
  oneway void reinitialize();

  /**
   * Suggest a shutdown to the server
   */
  oneway void shutdown();

  /**
   * Translate frame pointers to file name and line pairs.
   */
  list<string> translateFrames(1: list<i64> pointers);
}
