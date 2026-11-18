// Never run. windeployqt reads the Qt modules an executable imports to decide what to copy, but it
// takes every DLL whose name starts with "qt" for a Qt module and fails on vcpkg's qt6keychain.dll.
// quarry.exe imports it, so windeployqt scans this executable (same Qt modules, nothing else)
// instead; see cmake/QuarryInstall.cmake.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    return app.exec();
}
