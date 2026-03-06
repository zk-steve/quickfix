/* -*- C++ -*- */

/****************************************************************************
** Copyright (c) 2001-2014
**
** This file is part of the QuickFIX FIX Engine
**
** This file may be distributed under the terms of the quickfixengine.org
** license as defined by quickfixengine.org and appearing in the file
** LICENSE included in the packaging of this file.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.quickfixengine.org/LICENSE for licensing information.
**
** Contact ask@quickfixengine.org if any conditions of this licensing are
** not clear to you.
**
****************************************************************************/

#ifndef FIX_SOCKETMONITOR_UNIX_H
#define FIX_SOCKETMONITOR_UNIX_H

#ifndef _MSC_VER

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <poll.h>
#include <queue>
#include <set>
#include <time.h>

#if defined(HAVE_IO_URING) && defined(__linux__)
#include <liburing.h>
#endif

#include "Utility.h"

namespace FIX {
/// Monitors events on a collection of sockets.
class SocketMonitor {
public:
  class Strategy;

  SocketMonitor(int timeout = 0);
  virtual ~SocketMonitor();

  bool addConnect(socket_handle socket);
  bool addRead(socket_handle socket);
  bool addWrite(socket_handle socket);
  bool drop(socket_handle socket);
  void signal(socket_handle socket);
  void unsignal(socket_handle socket);
  void block(Strategy &strategy, bool poll = 0, double timeout = 0.0);

  size_t numSockets() { return m_readSockets.size() - 1; }

private:
  typedef std::set<socket_handle> Sockets;
  typedef std::queue<socket_handle> Queue;

  void setsockopt();
  bool bind();
  bool listen();
  void buildSet(const Sockets &, struct pollfd *pfds, short events);
  inline int getTimeval(bool poll, double timeout);
  inline bool sleepIfEmpty(bool poll);

  void processRead(Strategy &, socket_handle socket_fd);
  void processWrite(Strategy &, socket_handle socket_fd);
  void processError(Strategy &, socket_handle socket_fd);
  void processPollList(Strategy &strategy, struct pollfd *pfds, unsigned pfds_size);

#if defined(HAVE_IO_URING) && defined(__linux__)
  bool setupIoUring();
  void teardownIoUring();
  bool blockIoUring(Strategy &strategy, bool should_poll, double timeout);
  bool armIoUringPoll(socket_handle socket, short events, unsigned char eventType, Sockets &armedSockets);
  bool armIoUringPollers();
  void handleIoUringCompletion(Strategy &strategy, io_uring_cqe *cqe);
#endif

  int m_timeout;
  clock_t m_ticks;

  socket_handle m_signal;
  socket_handle m_interrupt;
  Sockets m_connectSockets;
  Sockets m_readSockets;
  Sockets m_writeSockets;
  Queue m_dropped;

#if defined(HAVE_IO_URING) && defined(__linux__)
  bool m_useIoUring;
  bool m_ioUringUsesMultishot;
  struct io_uring m_ioUring;
  Sockets m_ioUringReadArmed;
  Sockets m_ioUringConnectArmed;
  Sockets m_ioUringWriteArmed;
#endif

public:
  class Strategy {
  public:
    virtual ~Strategy() {}
    virtual void onConnect(SocketMonitor &, socket_handle socket) = 0;
    virtual void onEvent(SocketMonitor &, socket_handle socket) = 0;
    virtual void onWrite(SocketMonitor &, socket_handle socket) = 0;
    virtual void onError(SocketMonitor &, socket_handle socket) = 0;
    virtual void onError(SocketMonitor &) = 0;
    virtual void onTimeout(SocketMonitor &) {}
  };
};
} // namespace FIX

#endif //_MSC_VER

#endif // FIX_SOCKETMONITOR_UNIX_H
