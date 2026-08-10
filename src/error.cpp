/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/error.hpp>

/*
 * The description table costs roughly 700 bytes of read-only storage. A Zephyr
 * build can drop it with CONFIG_ZEST_ERROR_STRINGS=n; a host build, which has no
 * Kconfig, always keeps it.
 */
#if !defined(CONFIG_ZEST) || defined(CONFIG_ZEST_ERROR_STRINGS)
#define ZEST_WITH_ERROR_STRINGS 1
#endif

namespace zest
{

std::string_view Error::message() const noexcept
{
#if defined(ZEST_WITH_ERROR_STRINGS)
	switch (number()) {
	case 0:
		return "success";
	case EINVAL:
		return "invalid argument";
	case ENODEV:
		return "no such device";
	case ENOMEM:
		return "out of memory";
	case ENOBUFS:
		return "no buffer space";
	case EIO:
		return "I/O error";
	case EBUSY:
		return "device or resource busy";
	case EACCES:
		return "permission denied";
	case EALREADY:
		return "operation already in progress";
	case ETIMEDOUT:
		return "timed out";
	case ENOTCONN:
		return "not connected";
	case ECONNRESET:
		return "connection reset by peer";
	case ECONNREFUSED:
		return "connection refused";
	case ECONNABORTED:
		return "connection aborted";
	case EHOSTUNREACH:
		return "host unreachable";
	case ENETDOWN:
		return "network is down";
	case ENAMETOOLONG:
		return "name too long";
	case E2BIG:
		return "argument list too long";
	case EMSGSIZE:
		return "message too long";
	case EBADMSG:
		return "bad message";
	case EILSEQ:
		return "illegal byte sequence";
	case ENODATA:
		return "no data available";
	case ENOENT:
		return "no such entry";
	case ENOTSUP:
		return "not supported";
	case EAGAIN:
		return "resource temporarily unavailable";
	case EINPROGRESS:
		return "operation in progress";
	case EBADF:
		return "bad file descriptor";
	case ENOMSG:
		return "no message of the desired type";
	case EOVERFLOW:
		return "value too large";
	case ERANGE:
		return "result out of range";
	case EEXIST:
		return "already exists";
	case ENOSPC:
		return "no space left on device";
	case EPIPE:
		return "broken pipe";
	case ESHUTDOWN:
		return "endpoint has been shut down";
	case EPROTONOSUPPORT:
		return "protocol not supported";
	case EINTR:
		return "interrupted";
	case EPERM:
		return "operation not permitted";
	case EFAULT:
		return "bad address";
	case ENOSYS:
		return "function not implemented";
	case ENOTTY:
		return "inappropriate I/O control operation";
	case EDOM:
		return "argument out of domain";
	case EADDRINUSE:
		return "address already in use";
	case EAFNOSUPPORT:
		return "address family not supported";
	case ENETUNREACH:
		return "network unreachable";
	default:
		break;
	}
	return code_ < 0 ? "unrecognized error" : "success";
#else
	return "error";
#endif
}

} /* namespace zest */
