#pragma once

#include <QRect>

class QPoint;

class UIADetector
{
public:
    static UIADetector &instance();

    QRect getElementRectAt(const QPoint &globalPos, void *currentWindowHandle) const;
    bool isAvailable() const;

private:
    UIADetector();
    ~UIADetector();

    UIADetector(const UIADetector &) = delete;
    UIADetector &operator=(const UIADetector &) = delete;

};
