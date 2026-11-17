#include "export_sink.h"

#include <QIODevice>

namespace quarry {

ByteSink::ByteSink(QIODevice &device)
    : m_device(device)
{ }

bool ByteSink::flush()
{
    if (m_failed) {
        m_buffer.clear();
        return false;
    }
    if (m_buffer.isEmpty())
        return true;
    const qint64 written = m_device.write(m_buffer);
    if (written != m_buffer.size())
        m_failed = true;
    else
        m_accepted += written;
    m_buffer.resize(0); // clear() would free the capacity that the next chunk reuses
    return !m_failed;
}

} // namespace quarry
