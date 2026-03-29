#include "screencapturer.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QScreen>
#include <QWindow>

#include <algorithm>
#include <utility>

#include <Windows.h>
#include <dwmapi.h>

namespace
{
constexpr int kSelectionMinimum = 4;
constexpr int kMagnifierGrid = 13;
constexpr int kMagnifierPixelSize = 10;
constexpr int kMagnifierSize = kMagnifierGrid * kMagnifierPixelSize;
constexpr int kInfoPadding = 8;

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto *payload = reinterpret_cast<std::pair<QVector<ScreenCapturer::NativeWindowInfo> *, WId> *>(lParam);
    if (!payload) {
        return TRUE;
    }

    if (!ScreenCapturer::isCandidateWindow(hwnd, payload->second)) {
        return TRUE;
    }

    const QRect logicalRect = ScreenCapturer::nativeWindowRectToLogical(hwnd);
    if (!logicalRect.isValid() || logicalRect.isEmpty()) {
        return TRUE;
    }

    payload->first->push_back({logicalRect});
    return TRUE;
}

QRect inflateRect(const QRect &rect, int value)
{
    return rect.adjusted(-value, -value, value, value);
}
} // namespace

ScreenCapturer::ScreenCapturer(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::Tool
                   | Qt::WindowStaysOnTopHint
                   | Qt::NoDropShadowWindowHint
                   | Qt::BypassWindowManagerHint);

    qApp->installNativeEventFilter(this);
    registerHotkey();
}

ScreenCapturer::~ScreenCapturer()
{
    unregisterHotkey();
    qApp->removeNativeEventFilter(this);
}

bool ScreenCapturer::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result);

    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") {
        return false;
    }

    const auto *msg = static_cast<MSG *>(message);
    if (!msg || msg->message != WM_HOTKEY || static_cast<int>(msg->wParam) != m_hotkeyId) {
        return false;
    }

    if (!m_captureActive) {
        beginCapture();
    }

    return true;
}

void ScreenCapturer::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    drawDesktop(painter);
    painter.fillRect(rect(), QColor(0, 0, 0, 96));

    const QRect selection = currentSelection();
    if (selection.isValid() && !selection.isEmpty()) {
        drawSelectionOverlay(painter, selection);
    } else if (m_hoveredRect.isValid() && !m_hoveredRect.isEmpty()) {
        drawHoverOverlay(painter, m_hoveredRect);
    }

    drawMagnifier(painter);
}

void ScreenCapturer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        endCapture();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        acceptCapture();
        return;
    default:
        break;
    }

    QWidget::keyPressEvent(event);
}

void ScreenCapturer::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_mouseGlobalPos = m_virtualGeometry.topLeft() + event->position().toPoint();
    if (QScreen *targetScreen = targetScreenForPoint(m_mouseGlobalPos)) {
        setScreen(targetScreen);
    }
    m_dragging = true;
    m_hasSelection = true;
    m_dragStart = event->position().toPoint();
    m_dragCurrent = m_dragStart;
    update();
}

void ScreenCapturer::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint localPos = event->position().toPoint();
    m_mouseGlobalPos = m_virtualGeometry.topLeft() + localPos;
    if (QScreen *targetScreen = targetScreenForPoint(m_mouseGlobalPos)) {
        setScreen(targetScreen);
    }

    if (m_dragging) {
        m_dragCurrent = localPos;
    } else {
        m_hoveredRect = hoveredWindowRect(m_mouseGlobalPos);
    }

    update();
}

void ScreenCapturer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    m_dragCurrent = event->position().toPoint();
    m_dragging = false;

    const QRect manualSelection = currentSelection();
    if ((!manualSelection.isValid() || manualSelection.width() < kSelectionMinimum
         || manualSelection.height() < kSelectionMinimum)
        && m_hoveredRect.isValid()) {
        m_dragStart = m_hoveredRect.topLeft() - m_virtualGeometry.topLeft();
        m_dragCurrent = m_hoveredRect.bottomRight() - m_virtualGeometry.topLeft();
        m_hasSelection = true;
    }

    update();
}

void ScreenCapturer::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        acceptCapture();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}

void ScreenCapturer::registerHotkey()
{
    if (m_hotkeyRegistered) {
        return;
    }

    m_hotkeyRegistered = RegisterHotKey(nullptr, m_hotkeyId, MOD_NOREPEAT, VK_F2);
}

void ScreenCapturer::unregisterHotkey()
{
    if (!m_hotkeyRegistered) {
        return;
    }

    UnregisterHotKey(nullptr, m_hotkeyId);
    m_hotkeyRegistered = false;
}

void ScreenCapturer::beginCapture()
{
    // Capture first, then show the overlay, so the overlay itself never pollutes the screenshot.
    refreshScreenSnapshots();
    refreshVisibleWindows();

    m_captureActive = true;
    m_dragging = false;
    m_hasSelection = false;
    m_dragStart = {};
    m_dragCurrent = {};
    m_hoveredRect = {};
    m_mouseGlobalPos = QCursor::pos();
    m_hoveredRect = hoveredWindowRect(m_mouseGlobalPos);

    if (QScreen *targetScreen = QGuiApplication::primaryScreen()) {
        setScreen(targetScreen);
    }
    setGeometry(m_virtualGeometry);
    move(m_virtualGeometry.topLeft());
    setCursor(Qt::CrossCursor);
    show();
    raise();
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    update();
}

void ScreenCapturer::endCapture()
{
    hide();
    unsetCursor();
    m_captureActive = false;
    m_dragging = false;
    m_hasSelection = false;
    m_hoveredRect = {};
    m_dragStart = {};
    m_dragCurrent = {};
}

void ScreenCapturer::acceptCapture()
{
    QRect selection = currentSelection();
    if ((!selection.isValid() || selection.width() < kSelectionMinimum || selection.height() < kSelectionMinimum)
        && m_hoveredRect.isValid()) {
        selection = m_hoveredRect;
    }

    if (!selection.isValid() || selection.width() < 1 || selection.height() < 1) {
        endCapture();
        return;
    }

    const QImage image = extractSelectionImage(selection.translated(m_virtualGeometry.topLeft()));
    if (!image.isNull()) {
        qApp->clipboard()->setImage(image);
    }

    endCapture();
}

void ScreenCapturer::refreshScreenSnapshots()
{
    m_screens.clear();
    m_virtualGeometry = {};
    m_fullScreenPixmap = {};
    m_fullScreenImage = {};
    m_fullScreenDevicePixelRatio = 1.0;

    const auto screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (!screen) {
            continue;
        }

        ScreenSnapshot snapshot;
        snapshot.screen = screen;
        snapshot.geometry = screen->geometry();
        snapshot.devicePixelRatio = screen->devicePixelRatio();
        m_screens.push_back(std::move(snapshot));
    }

    QScreen *primaryScreen = QGuiApplication::primaryScreen();
    if (!primaryScreen) {
        return;
    }

    m_virtualGeometry = primaryScreen->virtualGeometry();
    m_fullScreenPixmap = primaryScreen->grabWindow(0);
    m_fullScreenDevicePixelRatio = m_fullScreenPixmap.isNull()
                                       ? primaryScreen->devicePixelRatio()
                                       : m_fullScreenPixmap.devicePixelRatio();

    if (!m_virtualGeometry.isValid() || m_virtualGeometry.isEmpty()) {
        return;
    }

    m_fullScreenImage = m_fullScreenPixmap.toImage();
}

void ScreenCapturer::refreshVisibleWindows()
{
    m_windows.clear();
    std::pair<QVector<NativeWindowInfo> *, WId> payload{&m_windows, winId()};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&payload));

    // Smaller rects first gives the snapping logic a better "nearest meaningful target" feel.
    std::sort(m_windows.begin(), m_windows.end(), [](const NativeWindowInfo &lhs, const NativeWindowInfo &rhs) {
        return lhs.rect.width() * lhs.rect.height() < rhs.rect.width() * rhs.rect.height();
    });
}

QRect ScreenCapturer::currentSelection() const
{
    if (!m_hasSelection) {
        return {};
    }

    return normalizedSelection(m_dragStart, m_dragCurrent);
}

QRect ScreenCapturer::normalizedSelection(const QPoint &start, const QPoint &end) const
{
    return QRect(start, end).normalized();
}

QRect ScreenCapturer::hoveredWindowRect(const QPoint &globalLogicalPos) const
{
    for (auto it = m_windows.cbegin(); it != m_windows.cend(); ++it) {
        if (it->rect.contains(globalLogicalPos)) {
            return it->rect.translated(-m_virtualGeometry.topLeft());
        }
    }

    return {};
}

QRect ScreenCapturer::logicalToDevice(const QRect &rect) const
{
    if (!rect.isValid() || rect.isEmpty()) {
        return {};
    }

    const QRect translated = rect.translated(-m_virtualGeometry.topLeft());
    const qreal dpr = m_fullScreenDevicePixelRatio;

    return QRect(qRound(translated.x() * dpr),
                 qRound(translated.y() * dpr),
                 std::max(1, qRound(translated.width() * dpr)),
                 std::max(1, qRound(translated.height() * dpr)));
}

QPoint ScreenCapturer::logicalToDevice(const QPoint &point) const
{
    const qreal dpr = m_fullScreenDevicePixelRatio;
    const QPoint translated = point - m_virtualGeometry.topLeft();
    return QPoint(qRound(translated.x() * dpr), qRound(translated.y() * dpr));
}

QScreen *ScreenCapturer::targetScreenForPoint(const QPoint &globalLogicalPos) const
{
    for (const auto &snapshot : m_screens) {
        if (snapshot.screen && snapshot.geometry.contains(globalLogicalPos)) {
            return snapshot.screen;
        }
    }

    return QGuiApplication::screenAt(globalLogicalPos);
}

qreal ScreenCapturer::devicePixelRatioForPoint(const QPoint &globalLogicalPos) const
{
    if (QScreen *screen = targetScreenForPoint(globalLogicalPos)) {
        return screen->devicePixelRatio();
    }

    return screen() ? screen()->devicePixelRatio() : m_fullScreenDevicePixelRatio;
}

void ScreenCapturer::drawDesktop(QPainter &painter, const QRect &targetClip) const
{
    if (targetClip.isValid() && !targetClip.isEmpty()) {
        painter.save();
        painter.setClipRect(targetClip);
    }

    if (!m_fullScreenPixmap.isNull()) {
        painter.drawPixmap(rect(), m_fullScreenPixmap);
    }

    if (targetClip.isValid() && !targetClip.isEmpty()) {
        painter.restore();
    }
}

void ScreenCapturer::drawSelectionOverlay(QPainter &painter, const QRect &selection)
{
    drawDesktop(painter, selection);

    painter.save();
    painter.setPen(QPen(QColor(0, 174, 255), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(selection);

    const QString sizeText = QStringLiteral("%1 x %2").arg(selection.width()).arg(selection.height());
    QFontMetrics metrics(font());
    const QRect textRect = inflateRect(metrics.boundingRect(sizeText), kInfoPadding)
                               .translated(selection.topLeft() + QPoint(0, -metrics.height() - 16));

    QRect adjustedTextRect = textRect;
    if (adjustedTextRect.top() < 0) {
        adjustedTextRect.moveTop(selection.bottom() + 12);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(30, 30, 30, 220));
    painter.drawRoundedRect(adjustedTextRect, 6, 6);
    painter.setPen(Qt::white);
    painter.drawText(adjustedTextRect, Qt::AlignCenter, sizeText);
    painter.restore();
}

void ScreenCapturer::drawHoverOverlay(QPainter &painter, const QRect &hoveredRect)
{
    painter.save();
    painter.setPen(QPen(QColor(255, 180, 0), 2, Qt::DashLine));
    painter.setBrush(QColor(255, 180, 0, 32));
    painter.drawRect(hoveredRect);
    painter.restore();
}

void ScreenCapturer::drawMagnifier(QPainter &painter)
{
    if (!rect().contains(mapFromGlobal(m_mouseGlobalPos))) {
        return;
    }

    QImage magnifierImage(kMagnifierSize, kMagnifierSize, QImage::Format_ARGB32_Premultiplied);
    magnifierImage.fill(QColor(24, 24, 24, 230));

    QPainter magnifierPainter(&magnifierImage);
    magnifierPainter.setRenderHint(QPainter::Antialiasing, false);

    const int halfGrid = kMagnifierGrid / 2;
    for (int y = -halfGrid; y <= halfGrid; ++y) {
        for (int x = -halfGrid; x <= halfGrid; ++x) {
            const QPoint samplePoint = m_mouseGlobalPos + QPoint(x, y);
            magnifierPainter.fillRect((x + halfGrid) * kMagnifierPixelSize,
                                      (y + halfGrid) * kMagnifierPixelSize,
                                      kMagnifierPixelSize,
                                      kMagnifierPixelSize,
                                      sampleColor(samplePoint));
        }
    }

    magnifierPainter.setPen(QPen(QColor(255, 255, 255, 80), 1));
    for (int i = 0; i <= kMagnifierGrid; ++i) {
        const int pos = i * kMagnifierPixelSize;
        magnifierPainter.drawLine(pos, 0, pos, kMagnifierSize);
        magnifierPainter.drawLine(0, pos, kMagnifierSize, pos);
    }

    magnifierPainter.setPen(QPen(QColor(255, 60, 60), 2));
    const QRect centerRect(halfGrid * kMagnifierPixelSize,
                           halfGrid * kMagnifierPixelSize,
                           kMagnifierPixelSize,
                           kMagnifierPixelSize);
    magnifierPainter.drawRect(centerRect);
    magnifierPainter.end();

    const QColor color = sampleColor(m_mouseGlobalPos);
    const QString infoText = QStringLiteral("RGB(%1, %2, %3)\nX:%4 Y:%5")
                                 .arg(color.red())
                                 .arg(color.green())
                                 .arg(color.blue())
                                 .arg(m_mouseGlobalPos.x())
                                 .arg(m_mouseGlobalPos.y());

    QFontMetrics metrics(font());
    QRect textRect = metrics.boundingRect(QRect(0, 0, 220, 60), Qt::TextWordWrap, infoText);
    textRect = inflateRect(textRect, kInfoPadding);

    QPoint anchor = mapFromGlobal(m_mouseGlobalPos) + QPoint(24, 24);
    QRect magnifierRect(anchor, QSize(kMagnifierSize, kMagnifierSize));
    QRect infoRect(anchor + QPoint(0, kMagnifierSize + 8), textRect.size());

    if (infoRect.right() > rect().right()) {
        const int shift = infoRect.right() - rect().right() + 12;
        magnifierRect.translate(-shift, 0);
        infoRect.translate(-shift, 0);
    }
    if (infoRect.bottom() > rect().bottom()) {
        const int shift = infoRect.bottom() - rect().bottom() + 12;
        magnifierRect.translate(0, -(shift + kMagnifierSize + 20));
        infoRect.translate(0, -(shift + kMagnifierSize + 20));
    }

    painter.save();
    painter.setPen(QPen(Qt::white, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawImage(magnifierRect, magnifierImage);
    painter.drawRect(magnifierRect);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(30, 30, 30, 220));
    painter.drawRoundedRect(infoRect, 6, 6);
    painter.setPen(Qt::white);
    painter.drawText(infoRect, Qt::AlignCenter | Qt::TextWordWrap, infoText);
    painter.restore();
}

QImage ScreenCapturer::extractSelectionImage(const QRect &globalLogicalSelection) const
{
    if (!globalLogicalSelection.isValid() || globalLogicalSelection.isEmpty()) {
        return {};
    }

    const QRect deviceRect = logicalToDevice(globalLogicalSelection);
    if (!deviceRect.isValid() || deviceRect.isEmpty() || m_fullScreenImage.isNull()) {
        return {};
    }

    const QRect bounded = deviceRect.intersected(m_fullScreenImage.rect());
    if (!bounded.isValid() || bounded.isEmpty()) {
        return {};
    }

    QImage output = m_fullScreenImage.copy(bounded);
    output.setDevicePixelRatio(devicePixelRatioForPoint(globalLogicalSelection.center()));
    return output;
}

QColor ScreenCapturer::sampleColor(const QPoint &globalLogicalPos) const
{
    if (m_fullScreenImage.isNull()) {
        return Qt::black;
    }

    const QPoint sourcePixel = logicalToDevice(globalLogicalPos);
    const QPoint boundedPoint(qBound(0, sourcePixel.x(), m_fullScreenImage.width() - 1),
                              qBound(0, sourcePixel.y(), m_fullScreenImage.height() - 1));
    if (m_fullScreenImage.rect().contains(boundedPoint)) {
        return m_fullScreenImage.pixelColor(boundedPoint);
    }

    return Qt::black;
}

bool ScreenCapturer::isCandidateWindow(void *handle, WId selfWinId)
{
    const auto hwnd = static_cast<HWND>(handle);
    if (!hwnd || reinterpret_cast<WId>(hwnd) == selfWinId) {
        return false;
    }

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) != 0 || (exStyle & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return false;
    }

    return (rect.right - rect.left) > 1 && (rect.bottom - rect.top) > 1;
}

QRect ScreenCapturer::nativeWindowRectToLogical(void *handle)
{
    const auto hwnd = static_cast<HWND>(handle);

    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        if (!GetWindowRect(hwnd, &rect)) {
            return {};
        }
    }

    using PhysicalToLogicalPointForPerMonitorDPIFn = BOOL(WINAPI *)(HWND, LPPOINT);
    static const auto convertPoint = reinterpret_cast<PhysicalToLogicalPointForPerMonitorDPIFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "PhysicalToLogicalPointForPerMonitorDPI"));

    POINT topLeft{rect.left, rect.top};
    POINT bottomRight{rect.right, rect.bottom};

    if (convertPoint && convertPoint(hwnd, &topLeft) && convertPoint(hwnd, &bottomRight)) {
        return QRect(QPoint(topLeft.x, topLeft.y), QPoint(bottomRight.x, bottomRight.y)).normalized();
    }

    const UINT dpi = GetDpiForWindow(hwnd);
    const qreal scale = dpi > 0 ? static_cast<qreal>(dpi) / 96.0 : 1.0;

    return QRect(QPoint(qRound(rect.left / scale), qRound(rect.top / scale)),
                 QPoint(qRound(rect.right / scale), qRound(rect.bottom / scale)))
        .normalized();
}
