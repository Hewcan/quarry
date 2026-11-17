#pragma once

// The ODBC headers in the order every platform needs: windows.h first (and only) on Windows,
// where sql.h relies on its types; unixODBC is self-contained.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>

// The W functions take UTF-16 code units. unixODBC can be built with SQLWCHAR = wchar_t (4 bytes
// on Linux and macOS, SQL_WCHART_CONVERT); the vcpkg build does not, and this code does not
// support it.
static_assert(sizeof(SQLWCHAR) == 2, "SQLWCHAR must be a 16-bit code unit");
