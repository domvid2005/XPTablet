// XPTablet: the binary CEF distribution replaces this file with Chromium's full list of network error codes; the source tree
// only points at Chromium. Only the enum's names come from it (cef_errorcode_t is an int either way), so a few are enough here.
NET_ERROR(IO_PENDING, -1)
NET_ERROR(FAILED, -2)
NET_ERROR(ABORTED, -3)
NET_ERROR(INVALID_ARGUMENT, -4)
NET_ERROR(INVALID_HANDLE, -5)
NET_ERROR(FILE_NOT_FOUND, -6)
NET_ERROR(TIMED_OUT, -7)
NET_ERROR(CONNECTION_REFUSED, -102)
NET_ERROR(NAME_NOT_RESOLVED, -105)
NET_ERROR(INTERNET_DISCONNECTED, -106)
