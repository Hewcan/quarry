#include "about_info.h"

#include <QCoreApplication>
#include <QFile>

namespace quarry::ui {

AboutInfo::AboutInfo(QObject *parent)
    : QObject(parent)
    , m_licensesPath(defaultLicensesPath())
{ }

QString AboutInfo::defaultLicensesPath()
{
    return QStringLiteral(":/licenses/THIRD_PARTY_LICENSES.txt");
}

void AboutInfo::setLicensesPath(const QString &path)
{
    if (m_licensesPath == path)
        return;
    m_licensesPath = path;
    emit licensesPathChanged();
}

QString AboutInfo::licenseText() const
{
    return readLicenses(m_licensesPath);
}

QString AboutInfo::readLicenses(const QString &path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(file.readAll());
        if (!text.isEmpty())
            return text;
    }
    return QCoreApplication::translate(
        "AboutInfo",
        "The list of third-party licenses is not available in this build of Quarry. It is in the "
        "THIRD_PARTY_LICENSES.txt file that comes with the installation.");
}

} // namespace quarry::ui
