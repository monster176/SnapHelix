#include "coordinateconverter.h"

#include <QGuiApplication>
#include <QScreen>
#include <QtMath>

#include <limits>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

namespace
{
QRect physicalRectFromMonitorInfo(const MONITORINFO &monitorInfo)
{
    return QRect(monitorInfo.rcMonitor.left,
                 monitorInfo.rcMonitor.top,
                 monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                 monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top);
}

QRect physicalRectForScreen(const QScreen *screen)
{
    if (!screen) {
        return {};
    }

    const QRect logicalGeometry = screen->geometry();
    const qreal dpr = screen->devicePixelRatio();
    return QRect(qRound(logicalGeometry.x() * dpr),
                 qRound(logicalGeometry.y() * dpr),
                 qRound(logicalGeometry.width() * dpr),
                 qRound(logicalGeometry.height() * dpr));
}

bool queryMonitorInfo(HMONITOR monitor, MONITORINFO &monitorInfo)
{
    if (!monitor) {
        return false;
    }

    monitorInfo.cbSize = sizeof(MONITORINFO);
    return GetMonitorInfoW(monitor, &monitorInfo) != FALSE;
}

QScreen *screenForMonitor(HMONITOR monitor, const QScreen *fallbackScreen)
{
    MONITORINFO monitorInfo{};
    if (!queryMonitorInfo(monitor, monitorInfo)) {
        return const_cast<QScreen *>(fallbackScreen ? fallbackScreen : QGuiApplication::primaryScreen());
    }

    const QRect monitorPhysicalRect = physicalRectFromMonitorInfo(monitorInfo);
    QScreen *bestScreen = nullptr;
    qint64 bestScore = std::numeric_limits<qint64>::max();

    for (QScreen *screen : QGuiApplication::screens()) {
        if (!screen) {
            continue;
        }

        const QRect candidateRect = physicalRectForScreen(screen);
        const qint64 score =
            std::abs(candidateRect.x() - monitorPhysicalRect.x())
            + std::abs(candidateRect.y() - monitorPhysicalRect.y())
            + std::abs(candidateRect.width() - monitorPhysicalRect.width())
            + std::abs(candidateRect.height() - monitorPhysicalRect.height());

        if (!bestScreen || score < bestScore) {
            bestScreen = screen;
            bestScore = score;
        }
    }

    return bestScreen ? bestScreen : const_cast<QScreen *>(fallbackScreen ? fallbackScreen
                                                                           : QGuiApplication::primaryScreen());
}
} // namespace

namespace CoordinateConverter
{
QRect monitorPhysicalRectForGlobalLogicalPoint(const QPoint &globalPos, const QScreen *preferredScreen)
{
    const QPoint physicalPoint = globalLogicalToPhysicalPoint(globalPos, preferredScreen);
    return monitorPhysicalRectForGlobalPhysicalPoint(physicalPoint, preferredScreen);
}

QRect monitorPhysicalRectForGlobalPhysicalPoint(const QPoint &physicalPoint, const QScreen *preferredScreen)
{
    const POINT winPoint{physicalPoint.x(), physicalPoint.y()};
    const HMONITOR monitor = MonitorFromPoint(winPoint, MONITOR_DEFAULTTONEAREST);
    Q_UNUSED(screenForMonitor(monitor, preferredScreen));

    MONITORINFO monitorInfo{};
    if (!queryMonitorInfo(monitor, monitorInfo)) {
        return {};
    }

    return physicalRectFromMonitorInfo(monitorInfo);
}

QPoint globalLogicalToPhysicalPoint(const QPoint &globalPos, const QScreen *preferredScreen)
{
    const auto fallbackPoint = POINT{globalPos.x(), globalPos.y()};
    const HMONITOR seedMonitor = MonitorFromPoint(fallbackPoint, MONITOR_DEFAULTTONEAREST);
    QScreen *screen = screenForMonitor(seedMonitor, preferredScreen);
    if (!screen) {
        return globalPos;
    }

    MONITORINFO monitorInfo{};
    if (!queryMonitorInfo(seedMonitor, monitorInfo)) {
        return globalPos;
    }

    qreal dpr = screen->devicePixelRatio();
    QPoint screenOrigin = screen->geometry().topLeft();
    QPoint physicalPoint(qRound((globalPos.x() - screenOrigin.x()) * dpr + monitorInfo.rcMonitor.left),
                         qRound((globalPos.y() - screenOrigin.y()) * dpr + monitorInfo.rcMonitor.top));

    const POINT winPoint{physicalPoint.x(), physicalPoint.y()};
    const HMONITOR resolvedMonitor = MonitorFromPoint(winPoint, MONITOR_DEFAULTTONEAREST);
    if (resolvedMonitor && resolvedMonitor != seedMonitor && queryMonitorInfo(resolvedMonitor, monitorInfo)) {
        screen = screenForMonitor(resolvedMonitor, screen);
        if (screen) {
            dpr = screen->devicePixelRatio();
            screenOrigin = screen->geometry().topLeft();
            physicalPoint = QPoint(qRound((globalPos.x() - screenOrigin.x()) * dpr + monitorInfo.rcMonitor.left),
                                   qRound((globalPos.y() - screenOrigin.y()) * dpr + monitorInfo.rcMonitor.top));
        }
    }

    return physicalPoint;
}

QRect physicalRectToLocalLogical(const QRect &globalPhysicalRect, const QScreen *targetScreen)
{
    if (!targetScreen || !globalPhysicalRect.isValid() || globalPhysicalRect.isEmpty()) {
        return {};
    }

    const POINT physicalCenter{
        globalPhysicalRect.center().x(),
        globalPhysicalRect.center().y(),
    };
    const HMONITOR monitor = MonitorFromPoint(physicalCenter, MONITOR_DEFAULTTONEAREST);
    QScreen *resolvedScreen = screenForMonitor(monitor, targetScreen);
    if (!resolvedScreen || resolvedScreen != targetScreen) {
        return {};
    }

    const QRect monitorRect = monitorPhysicalRectForGlobalPhysicalPoint(globalPhysicalRect.center(), resolvedScreen);
    if (!monitorRect.isValid()) {
        return {};
    }

    const qreal dpr = resolvedScreen->devicePixelRatio();
    if (dpr <= 0.0) {
        return {};
    }

    const qreal originX = static_cast<qreal>(monitorRect.x());
    const qreal originY = static_cast<qreal>(monitorRect.y());
    const qreal localLeft = (static_cast<qreal>(globalPhysicalRect.x()) - originX) / dpr;
    const qreal localTop = (static_cast<qreal>(globalPhysicalRect.y()) - originY) / dpr;
    const qreal localWidth = static_cast<qreal>(globalPhysicalRect.width()) / dpr;
    const qreal localHeight = static_cast<qreal>(globalPhysicalRect.height()) / dpr;

    return QRect(qRound(localLeft),
                 qRound(localTop),
                 qRound(localWidth),
                 qRound(localHeight));
}
} // namespace CoordinateConverter
