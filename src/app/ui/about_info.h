#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace quarry::ui {

// QML singleton `AboutInfo`: the text of the third-party licenses for the About dialog. Nothing is
// read until licenseText() is called, so it adds no work at startup.
class AboutInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The file licenseText() reads. A property so that tests, whose executable does not embed the
    // resource, can point it at a file of their own.
    Q_PROPERTY(QString licensesPath READ licensesPath WRITE setLicensesPath NOTIFY
                   licensesPathChanged FINAL)

public:
    explicit AboutInfo(QObject *parent = nullptr);

    static QString defaultLicensesPath();

    QString licensesPath() const { return m_licensesPath; }
    void setLicensesPath(const QString &path);

    // The licenses text, or a short explanation when the file cannot be read.
    Q_INVOKABLE QString licenseText() const;

    // `path` is read as UTF-8. Empty or unreadable files give the "not available" text.
    static QString readLicenses(const QString &path);

signals:
    void licensesPathChanged();

private:
    QString m_licensesPath;
};

} // namespace quarry::ui
