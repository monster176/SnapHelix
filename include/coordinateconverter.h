#pragma once

#include <QPoint>
#include <QRect>

class QScreen;

namespace CoordinateConverter
{
QPoint globalLogicalToPhysicalPoint(const QPoint &globalPos, const QScreen *preferredScreen = nullptr);
QRect monitorPhysicalRectForGlobalLogicalPoint(const QPoint &globalPos, const QScreen *preferredScreen = nullptr);
QRect monitorPhysicalRectForGlobalPhysicalPoint(const QPoint &physicalPoint, const QScreen *preferredScreen = nullptr);
QRect physicalRectToLocalLogical(const QRect &globalPhysicalRect, const QScreen *targetScreen);
}
