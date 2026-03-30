#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "uiadetector.h"
#include "coordinateconverter.h"

#include <QGuiApplication>
#include <QPoint>
#include <QScreen>
#include <QString>

#include <Windows.h>
#include <dwmapi.h>
#include <UIAutomation.h>
#include <ole2.h>

namespace
{
QRect rectFromVariant(const VARIANT &propertyValue)
{
    if (propertyValue.vt != (VT_ARRAY | VT_R8) || propertyValue.parray == nullptr) {
        return {};
    }

    double *coords = nullptr;
    if (FAILED(SafeArrayAccessData(propertyValue.parray, reinterpret_cast<void **>(&coords))) || !coords) {
        return {};
    }

    const QRect result(static_cast<int>(coords[0]),
                       static_cast<int>(coords[1]),
                       static_cast<int>(coords[2]),
                       static_cast<int>(coords[3]));
    SafeArrayUnaccessData(propertyValue.parray);
    return result;
}

QRect cachedElementRect(IUIAutomationElement *element)
{
    if (!element) {
        return {};
    }

    VARIANT propertyValue;
    VariantInit(&propertyValue);
    QRect result;
    if (SUCCEEDED(element->GetCachedPropertyValue(UIA_BoundingRectanglePropertyId, &propertyValue))) {
        result = rectFromVariant(propertyValue);
    }
    VariantClear(&propertyValue);
    return result;
}

CONTROLTYPEID cachedControlType(IUIAutomationElement *element)
{
    CONTROLTYPEID controlType = UIA_CustomControlTypeId;
    if (!element || FAILED(element->get_CachedControlType(&controlType))) {
        return UIA_CustomControlTypeId;
    }
    return controlType;
}

CONTROLTYPEID currentControlType(IUIAutomationElement *element)
{
    CONTROLTYPEID controlType = UIA_CustomControlTypeId;
    if (!element || FAILED(element->get_CurrentControlType(&controlType))) {
        return UIA_CustomControlTypeId;
    }
    return controlType;
}

QRect hwndRect(HWND hwnd)
{
    if (!hwnd) {
        return {};
    }

    RECT rect{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }
    if (GetWindowRect(hwnd, &rect)) {
        return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }
    return {};
}

bool isSelfOrChildWindow(HWND elementHwnd, HWND currentHwnd)
{
    if (!elementHwnd || !currentHwnd) {
        return false;
    }

    if (elementHwnd == currentHwnd || IsChild(currentHwnd, elementHwnd) || IsChild(elementHwnd, currentHwnd)) {
        return true;
    }

    HWND parent = GetParent(elementHwnd);
    while (parent) {
        if (parent == currentHwnd) {
            return true;
        }
        parent = GetParent(parent);
    }

    return false;
}

QString classNameForHwnd(HWND hwnd)
{
    if (!hwnd) {
        return {};
    }

    wchar_t buffer[256]{};
    const int len = GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return len > 0 ? QString::fromWCharArray(buffer, len) : QString{};
}

bool isNearlyFullScreen(const QRect &physicalRect, const QRect &screenGeometry, qreal dpr)
{
    if (!physicalRect.isValid() || !screenGeometry.isValid() || dpr <= 0.0) {
        return false;
    }

    const QSize screenPhysicalSize(qRound(screenGeometry.width() * dpr),
                                   qRound(screenGeometry.height() * dpr));
    return physicalRect.width() * 100 >= screenPhysicalSize.width() * 90
           || physicalRect.height() * 100 >= screenPhysicalSize.height() * 90;
}

bool isLargeContainer(CONTROLTYPEID controlType, const QRect &rect, const QRect &referenceRect)
{
    if (controlType != UIA_PaneControlTypeId
        && controlType != UIA_GroupControlTypeId
        && controlType != UIA_WindowControlTypeId) {
        return false;
    }

    if (!rect.isValid() || !referenceRect.isValid()) {
        return false;
    }

    const qint64 rectArea = static_cast<qint64>(rect.width()) * rect.height();
    const qint64 refArea = static_cast<qint64>(referenceRect.width()) * referenceRect.height();
    return refArea > 0 && rectArea * 100 >= refArea * 95;
}

bool isPreferredLeafType(CONTROLTYPEID controlType)
{
    return controlType == UIA_ImageControlTypeId
           || controlType == UIA_TextControlTypeId
           || controlType == UIA_ButtonControlTypeId
           || controlType == UIA_HyperlinkControlTypeId
           || controlType == UIA_MenuItemControlTypeId;
}

bool isElementVisible(IUIAutomationElement *element)
{
    if (!element) {
        return false;
    }

    BOOL offscreen = TRUE;
    return SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen;
}

QRect currentElementRect(IUIAutomationElement *element)
{
    if (!element) {
        return {};
    }

    VARIANT propertyValue;
    VariantInit(&propertyValue);
    QRect result;
    if (SUCCEEDED(element->GetCurrentPropertyValue(UIA_BoundingRectanglePropertyId, &propertyValue))) {
        result = rectFromVariant(propertyValue);
    }
    VariantClear(&propertyValue);
    return result;
}

IUIAutomationElement *pickSmallestChildAtPoint(IUIAutomation *automation,
                                               IUIAutomationElement *root,
                                               const QPoint &physicalPoint,
                                               int depthRemaining)
{
    if (!automation || !root || depthRemaining <= 0) {
        return nullptr;
    }

    IUIAutomationTreeWalker *walker = nullptr;
    if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) {
        return nullptr;
    }

    IUIAutomationElement *best = nullptr;
    QRect bestRect;
    IUIAutomationElement *child = nullptr;

    if (SUCCEEDED(walker->GetFirstChildElement(root, &child))) {
        while (child) {
            const QRect childRect = currentElementRect(child);
            const CONTROLTYPEID childType = currentControlType(child);

            if (isElementVisible(child) && childRect.isValid() && childRect.contains(physicalPoint)) {
                IUIAutomationElement *deeper = pickSmallestChildAtPoint(automation, child, physicalPoint, depthRemaining - 1);
                IUIAutomationElement *candidate = deeper ? deeper : child;
                const QRect candidateRect = deeper ? currentElementRect(deeper) : childRect;
                const CONTROLTYPEID candidateType = deeper ? currentControlType(deeper) : childType;
                const qint64 candidateArea = static_cast<qint64>(candidateRect.width()) * candidateRect.height();
                const qint64 bestArea = static_cast<qint64>(bestRect.width()) * bestRect.height();

                if (candidateRect.isValid()
                    && (!best
                        || candidateArea < bestArea
                        || (candidateArea == bestArea && isPreferredLeafType(candidateType)))) {
                    if (best) {
                        best->Release();
                    }
                    best = candidate;
                    best->AddRef();
                    bestRect = candidateRect;
                }

                if (deeper) {
                    deeper->Release();
                }
            }

            IUIAutomationElement *sibling = nullptr;
            walker->GetNextSiblingElement(child, &sibling);
            child->Release();
            child = sibling;
        }
    }

    walker->Release();
    return best;
}
} // namespace

UIADetector &UIADetector::instance()
{
    static UIADetector detector;
    return detector;
}

UIADetector::UIADetector() {}

UIADetector::~UIADetector() {}

bool UIADetector::isAvailable() const
{
    return true;
}

QRect UIADetector::getElementRectAt(const QPoint &globalPos, void *currentWindowHandle) const
{
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (!SUCCEEDED(initResult) && initResult != RPC_E_CHANGED_MODE) {
        return {};
    }

    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation,
                                reinterpret_cast<void **>(&automation))) || !automation) {
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }

    QScreen *screen = QGuiApplication::screenAt(globalPos);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect monitorRect = CoordinateConverter::monitorPhysicalRectForGlobalLogicalPoint(globalPos, screen);
    if (!monitorRect.isValid()) {
        automation->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }

    const auto currentHwnd = static_cast<HWND>(currentWindowHandle);
    const QPoint physicalPointValue = CoordinateConverter::globalLogicalToPhysicalPoint(globalPos, screen);
    POINT physicalPoint{physicalPointValue.x(), physicalPointValue.y()};

    if (!screen) {
        automation->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }

    const qreal dpr = screen->devicePixelRatio();

    const HWND underCursor = WindowFromPoint(physicalPoint);
    if (!underCursor || underCursor == currentHwnd || isSelfOrChildWindow(underCursor, currentHwnd)) {
        automation->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }

    if (classNameForHwnd(underCursor) == QStringLiteral("#32769")) {
        automation->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }

    const QRect currentWindowRect = hwndRect(currentHwnd);
    const QRect referenceRect = currentWindowRect.isValid()
                                    ? currentWindowRect
                                    : monitorRect;

    IUIAutomationCacheRequest *cacheRequest = nullptr;
    if (FAILED(automation->CreateCacheRequest(&cacheRequest)) || !cacheRequest) {
        automation->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }

    cacheRequest->put_TreeScope(TreeScope_Element);
    cacheRequest->AddProperty(UIA_BoundingRectanglePropertyId);
    cacheRequest->AddProperty(UIA_ControlTypePropertyId);
    cacheRequest->AddProperty(UIA_IsOffscreenPropertyId);
    cacheRequest->AddProperty(UIA_NativeWindowHandlePropertyId);
    cacheRequest->AddProperty(UIA_NamePropertyId);

    IUIAutomationElement *element = nullptr;
    if (FAILED(automation->ElementFromPointBuildCache(physicalPoint, cacheRequest, &element)) || !element) {
        cacheRequest->Release();
        automation->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }
    cacheRequest->Release();

    BOOL isOffscreen = TRUE;
    if (FAILED(element->get_CachedIsOffscreen(&isOffscreen)) || isOffscreen) {
        element->Release();
        automation->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }

    QRect result = cachedElementRect(element);
    CONTROLTYPEID controlType = cachedControlType(element);

    if (IUIAutomationElement *deepestChild = pickSmallestChildAtPoint(
            automation, element, QPoint(physicalPoint.x, physicalPoint.y), 4)) {
        const QRect childRect = currentElementRect(deepestChild);
        if (childRect.isValid()) {
            element->Release();
            element = deepestChild;
            result = childRect;
            controlType = currentControlType(element);
        } else {
            deepestChild->Release();
        }
    }

    UIA_HWND nativeHwnd = nullptr;
    HWND hwnd = nullptr;
    if (SUCCEEDED(element->get_CachedNativeWindowHandle(&nativeHwnd)) && nativeHwnd) {
        hwnd = reinterpret_cast<HWND>(nativeHwnd);
        if (isSelfOrChildWindow(hwnd, currentHwnd)) {
            element->Release();
            automation->Release();
            if (shouldUninitialize) {
                CoUninitialize();
            }
            return {};
        }

        if (classNameForHwnd(hwnd) == QStringLiteral("#32769")) {
            element->Release();
            automation->Release();
            if (shouldUninitialize) {
                CoUninitialize();
            }
            return {};
        }
    }

    BSTR currentName = nullptr;
    if (SUCCEEDED(element->get_CachedName(&currentName)) && currentName) {
        const QString name = QString::fromWCharArray(currentName);
        SysFreeString(currentName);
        if (name.compare(QStringLiteral("Desktop"), Qt::CaseInsensitive) == 0) {
            element->Release();
            automation->Release();
            if (shouldUninitialize) {
                CoUninitialize();
            }
            return {};
        }
    }

    if (isLargeContainer(controlType, result, referenceRect)) {
        element->Release();
        automation->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return {};
    }

    if (hwnd && controlType == UIA_WindowControlTypeId) {
        RECT frameBounds{};
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameBounds, sizeof(frameBounds)))) {
            result = QRect(frameBounds.left,
                           frameBounds.top,
                           frameBounds.right - frameBounds.left,
                           frameBounds.bottom - frameBounds.top);
        }
    }

    if (isNearlyFullScreen(result, monitorRect, 1.0)) {
        result = {};
    }

    element->Release();
    automation->Release();
    if (shouldUninitialize) {
        CoUninitialize();
    }
    return result;
}
