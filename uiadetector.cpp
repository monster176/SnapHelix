#if 0
#include "uiadetector.h"

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

QRect elementRect(IUIAutomationElement *element)
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

bool elementIsOffscreen(IUIAutomationElement *element)
{
    if (!element) {
        return true;
    }

    BOOL offscreen = TRUE;
    return FAILED(element->get_CurrentIsOffscreen(&offscreen)) || offscreen;
}

CONTROLTYPEID elementControlType(IUIAutomationElement *element)
{
    CONTROLTYPEID controlType = UIA_CustomControlTypeId;
    if (!element || FAILED(element->get_CurrentControlType(&controlType))) {
        return UIA_CustomControlTypeId;
    }
    return controlType;
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

IUIAutomationElement *pickSmallestChildAtPoint(IUIAutomation *automation,
                                               IUIAutomationElement *root,
                                               const QPoint &physicalPoint,
                                               const QRect &referenceRect)
{
    if (!automation || !root) {
        return nullptr;
    }

    IUIAutomationTreeWalker *walker = nullptr;
    if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) {
        return nullptr;
    }

    IUIAutomationElement *best = root;
    best->AddRef();
    QRect bestRect = elementRect(root);

    IUIAutomationElement *child = nullptr;
    if (SUCCEEDED(walker->GetFirstChildElement(root, &child))) {
        while (child) {
            QRect childRect = elementRect(child);
            const CONTROLTYPEID controlType = elementControlType(child);
            const bool usable = !elementIsOffscreen(child)
                                && childRect.isValid()
                                && childRect.contains(physicalPoint)
                                && !isLargeContainer(controlType, childRect, referenceRect);

            if (usable) {
                IUIAutomationElement *deeper = pickSmallestChildAtPoint(automation, child, physicalPoint, referenceRect);
                IUIAutomationElement *candidate = deeper ? deeper : child;
                QRect candidateRect = elementRect(candidate);
                if (candidateRect.isValid()
                    && (!bestRect.isValid()
                        || candidateRect.width() * candidateRect.height() <= bestRect.width() * bestRect.height())) {
                    best->Release();
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

    return physicalRect.width() >= screenPhysicalSize.width() - 8
           && physicalRect.height() >= screenPhysicalSize.height() - 8;
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
} // namespace

UIADetector &UIADetector::instance()
{
    static UIADetector detector;
    return detector;
}

UIADetector::UIADetector()
{
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_comInitialized = SUCCEEDED(initResult);

    if (!SUCCEEDED(initResult) && initResult != RPC_E_CHANGED_MODE) {
        return;
    }

    IUIAutomation *automation = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,
                                   nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_IUIAutomation,
                                   reinterpret_cast<void **>(&automation)))) {
        m_automation = automation;
    }
}

UIADetector::~UIADetector()
{
    if (auto *automation = static_cast<IUIAutomation *>(m_automation)) {
        automation->Release();
        m_automation = nullptr;
    }

    if (m_comInitialized) {
        CoUninitialize();
    }
}

bool UIADetector::isAvailable() const
{
    return m_automation != nullptr;
}

QRect UIADetector::getElementRectAt(const QPoint &globalPos, void *currentWindowHandle) const
{
    auto *automation = static_cast<IUIAutomation *>(m_automation);
    if (!automation) {
        return {};
    }

    QScreen *screen = QGuiApplication::screenAt(globalPos);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    const QPoint screenOrigin = screen ? screen->geometry().topLeft() : QPoint{};
    const QPoint relativeLogicalPos = globalPos - screenOrigin;
    POINT physicalPoint{
        static_cast<LONG>(qRound((relativeLogicalPos.x() + screenOrigin.x()) * dpr)),
        static_cast<LONG>(qRound((relativeLogicalPos.y() + screenOrigin.y()) * dpr))
    };

    const auto currentHwnd = static_cast<HWND>(currentWindowHandle);
    HWND hUnder = WindowFromPoint(physicalPoint);
    if (hUnder == currentHwnd) {
        hUnder = GetWindow(hUnder, GW_HWNDNEXT);
    }
    if (!hUnder || isSelfOrChildWindow(hUnder, currentHwnd)) {
        return {};
    }

    IUIAutomationElement *element = nullptr;
    if (FAILED(automation->ElementFromHandle(hUnder, &element)) || !element) {
        return {};
    }

    const QRect currentWindowRect = hwndRect(currentHwnd);
    const QRect referenceRect = currentWindowRect.isValid()
                                    ? currentWindowRect
                                    : (screen ? QRect(screenOrigin.x(),
                                                      screenOrigin.y(),
                                                      qRound(screen->availableGeometry().width() * dpr),
                                                      qRound(screen->availableGeometry().height() * dpr))
                                              : QRect{});

    IUIAutomationElement *bestElement = element;
    if (!elementIsOffscreen(element)) {
        if (IUIAutomationElement *smaller = pickSmallestChildAtPoint(
                automation, element, QPoint(physicalPoint.x, physicalPoint.y), referenceRect)) {
            bestElement = smaller;
            element->Release();
        }
    }

    QRect result;
    UIA_HWND nativeHwnd = nullptr;
    HWND hwnd = nullptr;

    if (SUCCEEDED(bestElement->get_CurrentNativeWindowHandle(&nativeHwnd)) && nativeHwnd) {
        hwnd = reinterpret_cast<HWND>(nativeHwnd);
        if (isSelfOrChildWindow(hwnd, currentHwnd)) {
            bestElement->Release();
            return {};
        }

        const QString className = classNameForHwnd(hwnd);
        if (className == QStringLiteral("#32769")) {
            bestElement->Release();
            return {};
        }
    }

    BSTR currentName = nullptr;
    if (SUCCEEDED(bestElement->get_CurrentName(&currentName)) && currentName) {
        const QString name = QString::fromWCharArray(currentName);
        SysFreeString(currentName);
        if (name.compare(QStringLiteral("Desktop"), Qt::CaseInsensitive) == 0) {
            bestElement->Release();
            return {};
        }
    }

    result = elementRect(bestElement);

    const CONTROLTYPEID controlType = elementControlType(bestElement);
    if (isLargeContainer(controlType, result, referenceRect)) {
        bestElement->Release();
        return {};
    }

    // 只有当探测到的是顶级窗口时，才用 DWM 修正（解决阴影偏移）
    // 如果探测到的是子控件（按钮等），直接用 UIA 的矩形
    CONTROLTYPEID type = 0;
    bestElement->get_CurrentControlType(&type);

    if (hwnd && type == UIA_WindowControlTypeId) {
        RECT frameBounds{};
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameBounds, sizeof(frameBounds)))) {
            result = QRect(frameBounds.left, frameBounds.top,
                           frameBounds.right - frameBounds.left,
                           frameBounds.bottom - frameBounds.top);
        }
    } else {
        // 保持 UIA 探测到的小矩形
        result = elementRect(bestElement);
    }

    if (screen && isNearlyFullScreen(result, screen->availableGeometry(), dpr)) {
        result = {};
    }

    bestElement->Release();
    return result;
}
#endif

// Keep uiadetector.cpp as the translation-unit entry point while the
// implementation stays in a separate file for easier incremental refactors.
#include "uiadetector_impl.cpp"
