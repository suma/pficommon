// Copyright (c)2008-2011, Preferred Infrastructure Inc.
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
// 
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
// 
//     * Neither the name of Preferred Infrastructure nor the names of other
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "rpc_server.h"

#include <vector>
#include <algorithm>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>


#include "../../network/socket.h"
#include "../../system/syscall.h"
#include "../../concurrent/thread.h"

namespace {
const double kEventLoopDefaultIntervalSec = 5.0;
}

namespace pfi {
namespace network {
namespace mprpc {

const double rpc_server::kEventLoopMinimumIntervalSec = 0.01;

basic_server::basic_server() { }

basic_server::~basic_server()
{
  close();
}

bool basic_server::close()
{
  return sock.close();
}

bool basic_server::create(uint16_t port, int backlog)
{
  return sock.listen(port, backlog);
}

rpc_server::rpc_server(double timeout_sec) :
  timeout_sec(timeout_sec),
  event_interval_sec(kEventLoopDefaultIntervalSec),
  serv_running(false)
#if defined(HAVE_EVENT) || defined(HAVE_EVENT_H)
  , ev_base(NULL)
  , accept_queue(4096)
#endif
{ }

rpc_server::~rpc_server()
{
#if defined(HAVE_EVENT) || defined(HAVE_EVENT_H)
  if (ev_base) {
    ::event_base_free(ev_base);
  }
#endif
}

void rpc_server::set_event_interval(double interval_sec)
{
  // set interval seconds of event loop and pcbuf
  event_interval_sec = std::max(kEventLoopMinimumIntervalSec, interval_sec);
}

double rpc_server::event_interval() const
{
  return event_interval_sec;
}

bool rpc_server::serv(uint16_t port, int nthreads)
{
  using pfi::lang::shared_ptr;
  using pfi::concurrent::thread;

  if (!basic_server::create(port))
    return false;

  serv_running = true;

  std::vector<shared_ptr<thread> > ths(nthreads);
  for (int i=0; i<nthreads; i++) {
    ths[i] = shared_ptr<thread>(new thread(
          pfi::lang::bind(&rpc_server::process, this)));
    if (!ths[i]->start()) return false;
  }

#if defined(HAVE_EVENT) || defined(HAVE_EVENT_H)
  ev_base = ::event_base_new();
  if (!ev_base)
    return false;

  // divide seconds to seconds[time_t] and microsecondes[suseconds_t]
  timeval ev_timeout;
  ev_timeout.tv_sec = static_cast<time_t>(event_interval_sec);
  ev_timeout.tv_usec = static_cast<suseconds_t>((event_interval_sec - ev_timeout.tv_sec) * 1e+6);

  // event loop with timeout
  while (serv_running) {
    ::event_base_once(ev_base, sock.get(), EV_READ, accept_event, this, &ev_timeout);
    ::event_base_loop(ev_base, EVLOOP_ONCE);
  }
#endif

  for (int i=0; i<nthreads; i++) {
    ths[i]->join();
  }

  return true;
}

bool rpc_server::running() const
{
  return serv_running;
}

void rpc_server::stop()
{
#if defined(HAVE_EVENT) || defined(HAVE_EVENT_H)
  if (serv_running && ev_base) {
    ::event_base_loopexit(ev_base, NULL);
  }
#endif
  serv_running = false;
  close();
}

void rpc_server::process()
{
  while(serv_running) {
    int s = -1;
#if defined(HAVE_EVENT) || defined(HAVE_EVENT_H)
    if (!accept_queue.pop(s, event_interval_sec))
      continue;
#else
    NO_INTR(s, ::accept(sock.get(), NULL, NULL));
    if (FAILED(s)) { continue; }
#endif
    socket ns(s);

    ns.set_nodelay(true);
    if(timeout_sec > 0) {
      if(!ns.set_timeout(timeout_sec)) {
        continue;
      }
    }

    pfi::lang::shared_ptr<rpc_stream> rs(new rpc_stream(ns.get(), timeout_sec));
    ns.release();

    while(serv_running) {
      rpc_message msg;
      try {
        if(!rs->receive(&msg)) {
          break;
        }
      } catch (const rpc_timeout_error&) {
        break;
      }

      if(!msg.is_request()) {
        continue;
      }

      rpc_request req(msg);
      try {
        process_request(req, rs);
      } catch (const rpc_error&e) {
      }
    }
  }
}

#if defined(HAVE_EVENT) || defined(HAVE_EVENT_H)
void rpc_server::accept_event(int listen_fd, short event, void *arg)
{
  if (event & EV_READ) {
    int s;
    rpc_server *server = reinterpret_cast<rpc_server*>(arg);
    NO_INTR(s, ::accept(listen_fd, NULL, NULL));
    if (FAILED(s)) return;

    server->accept_queue.push(s);
  }
}
#endif

void rpc_server::add(const std::string &name,
    pfi::lang::shared_ptr<invoker_base> invoker)
{
  funcs[name] = invoker;
}

void rpc_server::process_request(rpc_request& req, pfi::lang::shared_ptr<rpc_stream> rs)
{
  responder res(req.msgid, rs);

  std::map<std::string, pfi::lang::shared_ptr<invoker_base> >::iterator
    fun = funcs.find(req.method);

  if(fun == funcs.end()) {
    res.send_error((unsigned int)METHOD_NOT_FOUND, req.method);
    return;
  }

  try {
    fun->second->invoke(req, res);
  } catch (const rpc_error &e) {
  } catch (const std::exception &e) {
    res.send_error(std::string(e.what()));
  }
}


}  // namespace mprpc
}  // namespace network
}  // namespace pfi

