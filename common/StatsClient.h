/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright 2005 - 2016 Zarafa and its licensors
 */

#ifndef __STATSCLIENT_H__
#define __STATSCLIENT_H__

#include <kopano/zcdefs.h>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <ctime>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <kopano/ECLogger.h>
#include <kopano/memory.hpp>

namespace KC {

class _kc_export StatsClient _kc_final {
private:
	int fd = -1;
	struct sockaddr_un addr{};
	int addr_len = 0;
	bool thread_running = false;
	pthread_t countsSubmitThread{};
public:
	std::atomic<bool> terminate{false};
	std::mutex mapsLock;
	std::map<std::string, double> countsMapDouble;
	std::map<std::string, int64_t> countsMapInt64;

	~StatsClient();

	int startup(const std::string &collector);
	void countInc(const std::string & key, const std::string & key_sub);
	_kc_hidden void countAdd(const std::string &key, const std::string &key_sub, double n);
	void countAdd(const std::string & key, const std::string & key_sub, const int64_t n);
	_kc_hidden void submit(const std::string &key, time_t ts, double value);
	_kc_hidden void submit(const std::string &key, time_t ts, int64_t value);
};

} /* namespace */

#endif
