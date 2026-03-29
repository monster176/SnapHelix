#include <QApplication>
#include <QList>

#include "screencapturer.h"

int main(int argc, char *argv[])
{
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    QList<ScreenCapturer *> m_capturers = ScreenCapturer::createCapturers();
    Q_UNUSED(m_capturers);

    return app.exec();
}
