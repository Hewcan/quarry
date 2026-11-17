#pragma once

#include <QByteArray>
#include <QByteArrayView>

class QIODevice;

namespace quarry {

// Write buffer in front of one QIODevice: cells are appended here and reach the device in large
// chunks, so no per-cell device call or QTextStream is involved. After the first device error
// everything is dropped and failed() stays true.
class ByteSink
{
public:
    static constexpr qsizetype kFlushThreshold = 256 * 1024;

    explicit ByteSink(QIODevice &device);

    void append(QByteArrayView data)
    {
        m_buffer.append(data);
        flushIfFull();
    }
    void append(char c)
    {
        m_buffer.append(c);
        flushIfFull();
    }
    bool flush();

    bool failed() const noexcept { return m_failed; }
    // Bytes accepted so far (buffered or written).
    qint64 bytes() const noexcept { return m_accepted + m_buffer.size(); }

private:
    void flushIfFull()
    {
        if (m_buffer.size() >= kFlushThreshold)
            flush();
    }

    QIODevice &m_device;
    QByteArray m_buffer;
    qint64 m_accepted = 0; // bytes already handed to the device
    bool m_failed = false;
};

} // namespace quarry
