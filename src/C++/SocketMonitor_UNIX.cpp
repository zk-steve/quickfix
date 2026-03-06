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

#ifndef _MSC_VER

#include "config.h"

#include "SocketMonitor.h"
#include "Utility.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <set>

namespace FIX {
#if defined(HAVE_IO_URING) && defined(__linux__)
namespace {
enum IoUringEventType : unsigned char {
  IoUringReadEvent = 1,
  IoUringConnectEvent = 2,
  IoUringWriteEvent = 3
};

bool envValueIsFalse(const char *value) {
  if (!value || !*value) {
    return false;
  }

  return strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0
         || strcmp(value, "off") == 0 || strcmp(value, "OFF") == 0;
}

bool envValueIsTrue(const char *value) {
  if (!value || !*value) {
    return false;
  }

  return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0
         || strcmp(value, "on") == 0 || strcmp(value, "ON") == 0;
}

bool ioUringEnabledByEnv() {
  const char *enabled = std::getenv("QF_IO_URING");
  if (!enabled) {
    return true;
  }
  return !envValueIsFalse(enabled);
}

bool ioUringMultishotEnabledByEnv() {
  const char *enabled = std::getenv("QF_IO_URING_MULTISHOT");
  if (!enabled) {
    return true;
  }
  return envValueIsTrue(enabled);
}

unsigned int ioUringEntriesFromEnv() {
  const char *value = std::getenv("QF_IO_URING_ENTRIES");
  if (!value || !*value) {
    return 1024U;
  }

  char *end = 0;
  unsigned long parsed = std::strtoul(value, &end, 10);
  if (!end || *end != '\0' || parsed == 0UL) {
    return 1024U;
  }

  if (parsed > 8192UL) {
    return 8192U;
  }
  if (parsed < 64UL) {
    return 64U;
  }
  return static_cast<unsigned int>(parsed);
}

uint64_t encodeIoUringUserData(socket_handle socket, unsigned char eventType) {
  const uint32_t packedSocket = static_cast<uint32_t>(socket);
  return (static_cast<uint64_t>(eventType) << 32U) | static_cast<uint64_t>(packedSocket);
}

unsigned char decodeIoUringEventType(uint64_t userData) { return static_cast<unsigned char>(userData >> 32U); }

socket_handle decodeIoUringSocket(uint64_t userData) {
  const uint32_t packedSocket = static_cast<uint32_t>(userData & 0xFFFFFFFFULL);
  return static_cast<socket_handle>(packedSocket);
}
} // namespace
#endif

SocketMonitor::SocketMonitor(int timeout)
    : m_timeout(timeout)
#if defined(HAVE_IO_URING) && defined(__linux__)
      ,
      m_useIoUring(false),
      m_ioUringUsesMultishot(false),
      m_ioUring()
#endif
{
  socket_init();

  std::pair<socket_handle, socket_handle> sockets = socket_createpair();
  m_signal = sockets.first;
  m_interrupt = sockets.second;
  socket_setnonblock(m_signal);
  socket_setnonblock(m_interrupt);
  m_readSockets.insert(m_interrupt);

  m_ticks = clock();

#if defined(HAVE_IO_URING) && defined(__linux__)
  std::memset(&m_ioUring, 0, sizeof(m_ioUring));
  m_useIoUring = setupIoUring();
#endif
}

SocketMonitor::~SocketMonitor() {
#if defined(HAVE_IO_URING) && defined(__linux__)
  teardownIoUring();
#endif

  Sockets::iterator i;
  for (i = m_readSockets.begin(); i != m_readSockets.end(); ++i) {
    socket_close(*i);
  }

  socket_close(m_signal);
  socket_term();
}

bool SocketMonitor::addConnect(socket_handle s) {
  socket_setnonblock(s);
  Sockets::iterator i = m_connectSockets.find(s);
  if (i != m_connectSockets.end()) {
    return false;
  }

  m_connectSockets.insert(s);
  return true;
}

bool SocketMonitor::addRead(socket_handle s) {
  socket_setnonblock(s);
  Sockets::iterator i = m_readSockets.find(s);
  if (i != m_readSockets.end()) {
    return false;
  }

  m_readSockets.insert(s);
  return true;
}

bool SocketMonitor::addWrite(socket_handle s) {
  if (m_readSockets.find(s) == m_readSockets.end()) {
    return false;
  }

  socket_setnonblock(s);
  Sockets::iterator i = m_writeSockets.find(s);
  if (i != m_writeSockets.end()) {
    return false;
  }

  m_writeSockets.insert(s);
  return true;
}

bool SocketMonitor::drop(socket_handle s) {
  Sockets::iterator i = m_readSockets.find(s);
  Sockets::iterator j = m_writeSockets.find(s);
  Sockets::iterator k = m_connectSockets.find(s);

  if (i != m_readSockets.end() || j != m_writeSockets.end() || k != m_connectSockets.end()) {
    socket_close(s);
    m_readSockets.erase(s);
    m_writeSockets.erase(s);
    m_connectSockets.erase(s);
#if defined(HAVE_IO_URING) && defined(__linux__)
    m_ioUringReadArmed.erase(s);
    m_ioUringConnectArmed.erase(s);
    m_ioUringWriteArmed.erase(s);
#endif
    m_dropped.push(s);
    return true;
  }
  return false;
}

inline int SocketMonitor::getTimeval(bool poll, double timeout) {
  if (poll) {
    return 0;
  }

  timeout = m_timeout;

  if (!timeout) {
    return 0;
  }

  double elapsed = (double)(clock() - m_ticks) / (double)CLOCKS_PER_SEC;
  if (elapsed >= timeout || elapsed == 0.0) {
    m_ticks = clock();
    return (timeout * 1000);
  } else {
    return ((timeout - elapsed) * 1000);
  }
  return (timeout * 1000);
}

bool SocketMonitor::sleepIfEmpty(bool poll) {
  if (poll) {
    return false;
  }

  if (m_readSockets.empty() && m_writeSockets.empty() && m_connectSockets.empty()) {
    process_sleep(m_timeout);
    return true;
  } else {
    return false;
  }
}

void SocketMonitor::signal(socket_handle socket) { socket_send(m_signal, (char *)&socket, sizeof(socket)); }

void SocketMonitor::unsignal(socket_handle s) {
  Sockets::iterator i = m_writeSockets.find(s);
  if (i == m_writeSockets.end()) {
    return;
  }

  m_writeSockets.erase(s);

#if defined(HAVE_IO_URING) && defined(__linux__)
  m_ioUringWriteArmed.erase(s);
#endif
}

void SocketMonitor::block(Strategy &strategy, bool should_poll, double timeout) {
#if defined(HAVE_IO_URING) && defined(__linux__)
  if (m_useIoUring && blockIoUring(strategy, should_poll, timeout)) {
    return;
  }
#endif

  while (m_dropped.size()) {
    strategy.onError(*this, m_dropped.front());
    m_dropped.pop();
    if (m_dropped.size() == 0) {
      return;
    }
  }

  int pfds_size = m_readSockets.size() + m_connectSockets.size() + m_writeSockets.size();
  struct pollfd pfds[pfds_size];
  buildSet(m_readSockets, pfds, POLLPRI | POLLIN);
  buildSet(m_connectSockets, pfds + m_readSockets.size(), POLLOUT | POLLERR);
  buildSet(m_writeSockets, pfds + m_readSockets.size() + m_connectSockets.size(), POLLOUT);

  if (sleepIfEmpty(should_poll)) {
    strategy.onTimeout(*this);
    return;
  }

  int result;
  do {
    result = poll(pfds, pfds_size, getTimeval(should_poll, timeout));
  } while (result < 0 && errno == EINTR);

  if (result == 0) {
    strategy.onTimeout(*this);
    return;
  } else if (result > 0) {
    processPollList(strategy, pfds, pfds_size);
  } else {
    strategy.onError(*this);
  }
}

void SocketMonitor::processRead(Strategy &strategy, socket_handle socket_fd) {
  int s = socket_fd;
  if (s == m_interrupt) {
    socket_handle socket = 0;
    recv(s, (char *)&socket, sizeof(socket), 0);
    addWrite(socket);
  } else {
    strategy.onEvent(*this, s);
  }
}

void SocketMonitor::processWrite(Strategy &strategy, socket_handle socket_fd) {
  socket_handle s = socket_fd;
  if (m_connectSockets.find(s) != m_connectSockets.end()) {
    m_connectSockets.erase(s);
    m_readSockets.insert(s);
    strategy.onConnect(*this, s);
  } else {
    strategy.onWrite(*this, s);
  }
}

void SocketMonitor::processError(Strategy &strategy, socket_handle socket_fd) { strategy.onError(*this, socket_fd); }

void SocketMonitor::processPollList(Strategy &strategy, struct pollfd *pfds, unsigned pfds_size) {
  for (unsigned i = 0; i < pfds_size; ++i) {
    if ((pfds[i].revents & POLLIN) || (pfds[i].revents & POLLPRI)) {
      processRead(strategy, pfds[i].fd);
    }

    if ((pfds[i].revents & POLLOUT)) {
      processWrite(strategy, pfds[i].fd);
    }
    if ((pfds[i].revents & POLLERR)) {
      processError(strategy, pfds[i].fd);
    }
  }
}

void SocketMonitor::buildSet(const Sockets &sockets, struct pollfd *pfds, short events) {
  Sockets::const_iterator iter;
  unsigned int i = 0;
  for (iter = sockets.begin(); iter != sockets.end(); ++iter) {
    pfds[i].fd = *iter;
    pfds[i].events = events;
    pfds[i].revents = 0;
    i += 1;
  }
}

#if defined(HAVE_IO_URING) && defined(__linux__)
bool SocketMonitor::setupIoUring() {
  if (!ioUringEnabledByEnv()) {
    return false;
  }

  const unsigned int queueEntries = ioUringEntriesFromEnv();
  if (io_uring_queue_init(queueEntries, &m_ioUring, 0) < 0) {
    return false;
  }

  m_ioUringUsesMultishot = ioUringMultishotEnabledByEnv();
#ifndef IORING_POLL_ADD_MULTI
  m_ioUringUsesMultishot = false;
#endif
  return true;
}

void SocketMonitor::teardownIoUring() {
  if (!m_useIoUring) {
    return;
  }

  io_uring_queue_exit(&m_ioUring);
  m_useIoUring = false;
  m_ioUringUsesMultishot = false;
  m_ioUringReadArmed.clear();
  m_ioUringConnectArmed.clear();
  m_ioUringWriteArmed.clear();
}

bool SocketMonitor::armIoUringPoll(socket_handle socket, short events, unsigned char eventType, Sockets &armedSockets) {
  if (armedSockets.find(socket) != armedSockets.end()) {
    return true;
  }

  if (!socket_isValid(socket)) {
    return true;
  }

  io_uring_sqe *sqe = io_uring_get_sqe(&m_ioUring);
  if (!sqe) {
    if (io_uring_submit(&m_ioUring) < 0) {
      return false;
    }
    sqe = io_uring_get_sqe(&m_ioUring);
    if (!sqe) {
      return false;
    }
  }

  io_uring_prep_poll_add(sqe, socket, events);
#ifdef IORING_POLL_ADD_MULTI
  if (m_ioUringUsesMultishot && eventType == IoUringReadEvent) {
    sqe->len = IORING_POLL_ADD_MULTI;
  }
#endif
  sqe->user_data = encodeIoUringUserData(socket, eventType);
  armedSockets.insert(socket);
  return true;
}

bool SocketMonitor::armIoUringPollers() {
  for (Sockets::const_iterator iter = m_readSockets.begin(); iter != m_readSockets.end(); ++iter) {
    if (!armIoUringPoll(*iter, POLLPRI | POLLIN, IoUringReadEvent, m_ioUringReadArmed)) {
      return false;
    }
  }

  for (Sockets::const_iterator iter = m_connectSockets.begin(); iter != m_connectSockets.end(); ++iter) {
    if (!armIoUringPoll(*iter, POLLOUT | POLLERR, IoUringConnectEvent, m_ioUringConnectArmed)) {
      return false;
    }
  }

  for (Sockets::const_iterator iter = m_writeSockets.begin(); iter != m_writeSockets.end(); ++iter) {
    if (!armIoUringPoll(*iter, POLLOUT, IoUringWriteEvent, m_ioUringWriteArmed)) {
      return false;
    }
  }

  return io_uring_submit(&m_ioUring) >= 0;
}

void SocketMonitor::handleIoUringCompletion(Strategy &strategy, io_uring_cqe *cqe) {
  const socket_handle socket = decodeIoUringSocket(cqe->user_data);
  const unsigned char eventType = decodeIoUringEventType(cqe->user_data);

  Sockets *armedSockets = 0;
  bool shouldDispatch = false;
  if (eventType == IoUringReadEvent) {
    armedSockets = &m_ioUringReadArmed;
    shouldDispatch = m_readSockets.find(socket) != m_readSockets.end();
  } else if (eventType == IoUringConnectEvent) {
    armedSockets = &m_ioUringConnectArmed;
    shouldDispatch = m_connectSockets.find(socket) != m_connectSockets.end();
  } else if (eventType == IoUringWriteEvent) {
    armedSockets = &m_ioUringWriteArmed;
    shouldDispatch = m_writeSockets.find(socket) != m_writeSockets.end();
  }

  if (armedSockets) {
    bool keepArmed = false;
#ifdef IORING_CQE_F_MORE
    keepArmed = m_ioUringUsesMultishot && eventType == IoUringReadEvent && ((cqe->flags & IORING_CQE_F_MORE) != 0);
#endif
    if (!keepArmed) {
      armedSockets->erase(socket);
    }
  }

  if (cqe->res < 0) {
    if (cqe->res != -ECANCELED && cqe->res != -ENOENT && cqe->res != -EBADF) {
      processError(strategy, socket);
    }
    return;
  }

  if (!shouldDispatch) {
    return;
  }

  const short revents = static_cast<short>(cqe->res);
  if ((revents & POLLIN) || (revents & POLLPRI)) {
    processRead(strategy, socket);
  }

  if (revents & POLLOUT) {
    processWrite(strategy, socket);
  }

  if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
    processError(strategy, socket);
  }
}

bool SocketMonitor::blockIoUring(Strategy &strategy, bool should_poll, double timeout) {
  while (m_dropped.size()) {
    strategy.onError(*this, m_dropped.front());
    m_dropped.pop();
    if (m_dropped.size() == 0) {
      return true;
    }
  }

  if (sleepIfEmpty(should_poll)) {
    strategy.onTimeout(*this);
    return true;
  }

  if (!armIoUringPollers()) {
    teardownIoUring();
    return false;
  }

  io_uring_cqe *cqe = 0;
  __kernel_timespec ts;
  __kernel_timespec *timeoutTs = 0;
  if (should_poll) {
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    timeoutTs = &ts;
  } else {
    const int timeoutMs = getTimeval(should_poll, timeout);
    ts.tv_sec = timeoutMs / 1000;
    ts.tv_nsec = static_cast<long>((timeoutMs % 1000) * 1000000L);
    timeoutTs = &ts;
  }

  int result;
  do {
    result = io_uring_wait_cqe_timeout(&m_ioUring, &cqe, timeoutTs);
  } while (result == -EINTR);

  if (result == -ETIME) {
    strategy.onTimeout(*this);
    return true;
  }

  if (result < 0) {
    teardownIoUring();
    return false;
  }

  handleIoUringCompletion(strategy, cqe);
  io_uring_cqe_seen(&m_ioUring, cqe);

  while (io_uring_peek_cqe(&m_ioUring, &cqe) == 0) {
    handleIoUringCompletion(strategy, cqe);
    io_uring_cqe_seen(&m_ioUring, cqe);
  }

  return true;
}
#endif

} // namespace FIX

#endif
