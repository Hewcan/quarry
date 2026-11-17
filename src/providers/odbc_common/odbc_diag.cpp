#include "odbc_diag.h"

#include "odbc_support.h"
#include "odbc_text.h"

#include <algorithm>

namespace quarry::odbc {

namespace {

constexpr SQLSMALLINT kMessageChars = 2048;

bool isWarning(const Diagnostic &record)
{
    return record.sqlState.startsWith(QLatin1String("01"));
}

} // namespace

std::vector<Diagnostic> readDiagnostics(SQLSMALLINT handleType, SQLHANDLE handle)
{
    std::vector<Diagnostic> out;
    if (!handle)
        return out;
    std::vector<SQLWCHAR> message(kMessageChars);
    for (SQLSMALLINT record = 1; record < 64; ++record) {
        SQLWCHAR state[6] = {};
        SQLINTEGER native = 0;
        SQLSMALLINT length = 0;
        const SQLRETURN rc = SQLGetDiagRecW(handleType, handle, record, state, &native,
                                            message.data(), kMessageChars, &length);
        if (!succeeded(rc))
            break;
        Diagnostic diagnostic;
        diagnostic.sqlState = fromWide(state, 5);
        diagnostic.nativeError = static_cast<long>(native);
        // length is what the driver wanted to write: a longer text arrives truncated.
        diagnostic.message
            = fromWide(message.data(), std::min<qsizetype>(length, kMessageChars - 1));
        out.push_back(std::move(diagnostic));
    }
    return out;
}

Error errorFromDiagnostics(const std::vector<Diagnostic> &records)
{
    Error error;
    if (records.empty()) {
        error.kind = ErrorKind::Driver;
        error.message = messageForKind(ErrorKind::Driver);
        error.detail = Text::tr("The driver gave no details.");
        return error;
    }
    const Diagnostic *chosen = &records.front();
    for (const Diagnostic &record : records) {
        if (!isWarning(record)) {
            chosen = &record;
            break;
        }
    }
    error.kind = kindForDiagnostic(chosen->sqlState, chosen->nativeError);
    error.message = messageForKind(error.kind);
    error.sqlState = chosen->sqlState;
    QStringList lines;
    lines.reserve(static_cast<qsizetype>(records.size()));
    for (const Diagnostic &record : records) {
        lines << QStringLiteral("%1 (SQLSTATE %2, native error %3)")
                     .arg(record.message, record.sqlState)
                     .arg(record.nativeError);
    }
    error.detail = lines.join(QLatin1Char('\n'));
    return error;
}

Error errorFromHandle(SQLSMALLINT handleType, SQLHANDLE handle)
{
    return errorFromDiagnostics(readDiagnostics(handleType, handle));
}

Error connectError(const std::vector<Diagnostic> &records, const QString &target)
{
    Error error = errorFromDiagnostics(records);
    const QString &state = error.sqlState;
    // unixODBC reports a driver it cannot find as 01000 "Can't open lib ... file not found"
    // followed by IM003, and the first non-warning record is not always the IM one: look at every
    // record, by SQLSTATE and by text.
    const auto mentions = [&records](QLatin1String text) {
        return std::any_of(records.begin(), records.end(), [text](const Diagnostic &record) {
            return record.message.contains(text, Qt::CaseInsensitive);
        });
    };
    const auto hasState = [&records](QLatin1String code) {
        return std::any_of(records.begin(), records.end(),
                           [code](const Diagnostic &record) { return record.sqlState == code; });
    };
    if (state == QLatin1String("IM002") || hasState(QLatin1String("IM002"))
        || mentions(QLatin1String("data source name not found"))) {
        error.kind = ErrorKind::Connection;
        error.message
            = Text::tr("The ODBC data source or driver \"%1\" was not found.").arg(target);
    } else if (state == QLatin1String("IM003") || state == QLatin1String("IM004")
               || state == QLatin1String("IM010") || state == QLatin1String("IM014")
               || hasState(QLatin1String("IM003")) || mentions(QLatin1String("file not found"))
               || mentions(QLatin1String("could not be loaded"))) {
        error.kind = ErrorKind::Connection;
        error.message = Text::tr("The ODBC driver \"%1\" could not be loaded.").arg(target);
    } else if (error.kind == ErrorKind::Driver || error.kind == ErrorKind::Syntax
               || error.kind == ErrorKind::Unsupported) {
        error.kind = ErrorKind::Connection;
        error.message = Text::tr("Could not connect to the server.");
    }
    return error;
}

void collectNotices(SQLSMALLINT handleType, SQLHANDLE handle, QStringList &notices)
{
    for (const Diagnostic &record : readDiagnostics(handleType, handle)) {
        if (notices.size() >= kMaxNotices)
            return;
        // 01004: data truncated (handled by the reader); 01S02: the driver changed an option.
        if (record.sqlState == QLatin1String("01004") || record.sqlState == QLatin1String("01S02"))
            continue;
        const QString text = stripDriverPrefix(record.message);
        if (!text.isEmpty())
            notices << text;
    }
}

Error notOpenError()
{
    return Error { ErrorKind::Connection, Text::tr("The connection is not open."), {}, {}, {} };
}

Error busyError()
{
    return Error {
        ErrorKind::Internal, Text::tr("The connection is busy with another result."), {}, {}, {}
    };
}

Error cancelledError()
{
    return Error { ErrorKind::Cancelled,
                   messageForKind(ErrorKind::Cancelled),
                   QStringLiteral("HY008"),
                   QStringLiteral("HY008"),
                   {} };
}

Error internalError(const QString &text)
{
    return Error { ErrorKind::Internal, Text::tr("Something went wrong."), text, {}, {} };
}

} // namespace quarry::odbc
