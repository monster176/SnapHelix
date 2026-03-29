#include "screencapturer.h"
#include "coordinateconverter.h"
#include "uiadetector.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDebug>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScreen>
#include <QShowEvent>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <QtMath>

#include <algorithm>
#include <utility>

#include <Windows.h>
#include <dwmapi.h>

QList<ScreenCapturer *> ScreenCapturer::s_capturers;
QVector<ScreenCapturer::NativeWindowInfo> ScreenCapturer::s_windows;
bool ScreenCapturer::s_isCapturing = false;

namespace
{
constexpr int kSelectionMinimum = 4;
constexpr int kMagnifierGrid = 13;
constexpr int kMagnifierPixelSize = 10;
constexpr int kMagnifierSize = kMagnifierGrid * kMagnifierPixelSize;
constexpr int kInfoPadding = 8;
constexpr int kProbePixelThreshold = 5;
constexpr qint64 kProbeIntervalMs = 40;
constexpr int kAdjustMargin = 8;
constexpr int kHandleNone = -1;
constexpr int kHandleMove = 0;
constexpr int kHandleLeft = 1;
constexpr int kHandleTop = 2;
constexpr int kHandleRight = 3;
constexpr int kHandleBottom = 4;
constexpr int kHandleTopLeft = 5;
constexpr int kHandleTopRight = 6;
constexpr int kHandleBottomLeft = 7;
constexpr int kHandleBottomRight = 8;

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

QRect clampAdjustedRect(const QRect &rect, const QRect &bounds)
{
    if (!rect.isValid() || bounds.isEmpty()) {
        return {};
    }

    QRect normalized = rect.normalized();
    normalized.setLeft(qBound(bounds.left(), normalized.left(), bounds.right()));
    normalized.setTop(qBound(bounds.top(), normalized.top(), bounds.bottom()));
    normalized.setRight(qBound(bounds.left(), normalized.right(), bounds.right()));
    normalized.setBottom(qBound(bounds.top(), normalized.bottom(), bounds.bottom()));
    return normalized.normalized();
}

} // namespace

ScreenCapturer::ScreenCapturer(QScreen *screen, bool controllerInstance, QWidget *parent)
    : QWidget(parent)
    , m_screen(screen)
    , m_isController(controllerInstance)
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
    m_probeTimer.start();

    if (m_screen) {
        m_screenGeometry = m_screen->geometry();
        setGeometry(m_screenGeometry);
    }

    s_capturers.push_back(this);

    if (m_isController) {
        qApp->installNativeEventFilter(this);
        registerHotkey();
    }
}

ScreenCapturer::~ScreenCapturer()
{
    s_capturers.removeAll(this);
    s_isCapturing = false;

    if (m_isController) {
        unregisterHotkey();
        qApp->removeNativeEventFilter(this);
    }
}

QList<ScreenCapturer *> ScreenCapturer::createCapturers()
{
    QList<ScreenCapturer *> capturers;
    bool controllerAssigned = false;

    for (QScreen *screen : QGuiApplication::screens()) {
        if (!screen) {
            continue;
        }

        auto *capturer = new ScreenCapturer(screen, !controllerAssigned);
        capturer->hide();
        capturers.push_back(capturer);
        controllerAssigned = true;
    }

    return capturers;
}

bool ScreenCapturer::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result);

    if (!m_isController) {
        return false;
    }

    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") {
        return false;
    }

    const auto *msg = static_cast<MSG *>(message);
    if (!msg || msg->message != WM_HOTKEY || static_cast<int>(msg->wParam) != m_hotkeyId) {
        return false;
    }

    if (ScreenCapturer::s_isCapturing) {
        closeAllCaptures();
        return true;
    }

    beginAllCaptures();
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

void ScreenCapturer::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    if (windowHandle() && m_screen) {
        windowHandle()->setScreen(m_screen);
    }
}

void ScreenCapturer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        closeAllCaptures();
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
    m_mouseLocalPos = event->position().toPoint();
    m_mouseGlobalPos = event->globalPosition().toPoint();

    if (event->button() == Qt::RightButton) {
        m_isCaptured = false;
        m_isResizing = false;
        m_hoveredRect = {};
        m_activeHandle = kHandleNone;
        m_adjustPressPos = {};
        m_adjustStartRect = {};
        setCursor(Qt::CrossCursor);
        update();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int handle = hoverHandleAt(m_mouseLocalPos);
    if (m_hoveredRect.isValid() && !m_hoveredRect.isEmpty()) {
        m_isCaptured = true;
    }

    if (handle != kHandleNone) {
        m_isResizing = true;
        m_activeHandle = handle;
        m_adjustPressPos = m_mouseLocalPos;
        m_adjustStartRect = m_hoveredRect;
        m_isProbing = false;
        setInputTransparent(false);
        updateAdjustCursor(m_mouseLocalPos);
        update();
        return;
    }

    m_dragging = true;
    m_hasSelection = true;
    m_dragStart = m_mouseLocalPos;
    m_dragCurrent = m_dragStart;
    update();
}

void ScreenCapturer::mouseMoveEvent(QMouseEvent *event)
{
    m_mouseLocalPos = event->position().toPoint();
    m_mouseGlobalPos = event->globalPosition().toPoint();

    if (m_isCaptured && m_isResizing) {
        const QRect adjustedRect = adjustedHoveredRect(m_mouseLocalPos);
        if (adjustedRect.isValid() && !adjustedRect.isEmpty()) {
            m_hoveredRect = adjustedRect;
        }
        updateAdjustCursor(m_mouseLocalPos);
        update();
        return;
    }

    if (m_isCaptured) {
        updateAdjustCursor(m_mouseLocalPos);
        return;
    }

    if (m_dragging) {
        m_dragCurrent = m_mouseLocalPos;
        update();
        return;
    }

    updateAdjustCursor(m_mouseLocalPos);

    if (event->buttons() != Qt::NoButton) {
        return;
    }

    const QPoint delta = m_mouseGlobalPos - m_lastProbePos;
    if (std::abs(delta.x()) < kProbePixelThreshold && std::abs(delta.y()) < kProbePixelThreshold) {
        update();
        return;
    }

    if (m_probeTimer.elapsed() < kProbeIntervalMs) {
        update();
        return;
    }

    if (m_isProbing) {
        update();
        return;
    }

    m_isProbing = true;
    m_lastProbePos = m_mouseGlobalPos;
    m_probeTimer.restart();
    setInputTransparent(true);

    const QPoint probePos = m_mouseGlobalPos;
    const auto handleValue = static_cast<quintptr>(winId());
    QtConcurrent::run([this, probePos, handleValue]() {
        const QRect rect = UIADetector::instance().getElementRectAt(
            probePos, reinterpret_cast<void *>(handleValue));

        QMetaObject::invokeMethod(this,
                                  [this, rect, probePos]() {
                                      setInputTransparent(false);

                                      if (m_isCaptured) {
                                          m_isProbing = false;
                                          return;
                                      }

                                      if (rect.isValid() && rect != m_hoveredRect) {
                                          const QRect candidateRect = uiaPhysicalRectToLocalLogical(rect);
                                          if (shouldAcceptHoveredRect(candidateRect)) {
                                              m_hoveredRect = candidateRect;
                                              update();
                                          }
                                      } else if (!rect.isValid()) {
                                          const QRect candidateRect = hoveredWindowRect(probePos);
                                          if (shouldAcceptHoveredRect(candidateRect)) {
                                              m_hoveredRect = candidateRect;
                                              update();
                                          }
                                      }

                                      m_isProbing = false;
                                  },
                                  Qt::QueuedConnection);
    });

    update();
}

void ScreenCapturer::mouseReleaseEvent(QMouseEvent *event)
{
    m_mouseLocalPos = event->position().toPoint();

    if (event->button() == Qt::RightButton) {
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_isCaptured && m_isResizing) {
        const QRect adjustedRect = adjustedHoveredRect(m_mouseLocalPos);
        if (adjustedRect.isValid() && !adjustedRect.isEmpty()) {
            m_hoveredRect = adjustedRect;
        }
        m_isResizing = false;
        m_activeHandle = kHandleNone;
        m_adjustPressPos = {};
        m_adjustStartRect = {};
        updateAdjustCursor(m_mouseLocalPos);
        update();
        return;
    }

    m_isCaptured = m_hoveredRect.isValid() && !m_hoveredRect.isEmpty();
    m_isResizing = false;

    m_dragCurrent = event->position().toPoint();
    m_dragging = false;

    const QRect manualSelection = currentSelection();
    if ((!manualSelection.isValid() || manualSelection.width() < kSelectionMinimum
         || manualSelection.height() < kSelectionMinimum)
        && m_hoveredRect.isValid()) {
        m_dragStart = m_hoveredRect.topLeft();
        m_dragCurrent = m_hoveredRect.bottomRight();
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
    ScreenCapturer::s_isCapturing = true;
    refreshScreenSnapshot();
    if (!m_screen) {
        ScreenCapturer::s_isCapturing = false;
        return;
    }

    m_captureActive = true;
    m_dragging = false;
    m_hasSelection = false;
    m_dragStart = {};
    m_dragCurrent = {};
    m_hoveredRect = {};
    m_mouseGlobalPos = QCursor::pos();
    m_mouseLocalPos = m_mouseGlobalPos - m_screenGeometry.topLeft();
    m_lastProbePos = m_mouseGlobalPos;
    m_adjustPressPos = {};
    m_adjustStartRect = {};
    m_isCaptured = false;
    m_isResizing = false;
    m_activeHandle = kHandleNone;
    m_probeTimer.restart();

    setGeometry(m_screenGeometry);
    show();
    if (windowHandle()) {
        windowHandle()->setScreen(m_screen);
    }
    setCursor(Qt::CrossCursor);
    raise();

    if (m_screenGeometry.contains(QCursor::pos())) {
        activateWindow();
        setFocus(Qt::ActiveWindowFocusReason);
    }

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
    m_isProbing = false;
    m_adjustPressPos = {};
    m_adjustStartRect = {};
    m_isCaptured = false;
    m_isResizing = false;
    m_activeHandle = kHandleNone;
    setInputTransparent(false);
    ScreenCapturer::s_isCapturing = false;
}

void ScreenCapturer::acceptCapture()
{
    QRect selection = currentSelection();
    if ((!selection.isValid() || selection.width() < kSelectionMinimum || selection.height() < kSelectionMinimum)
        && m_hoveredRect.isValid()) {
        selection = m_hoveredRect;
    }

    if (!selection.isValid() || selection.width() < 1 || selection.height() < 1) {
        closeAllCaptures();
        return;
    }

    const QImage image = extractSelectionImage(selection);
    if (!image.isNull()) {
        qApp->clipboard()->setImage(image);
    }

    closeAllCaptures();
}

void ScreenCapturer::refreshScreenSnapshot()
{
    if (!m_screen) {
        return;
    }

    m_screenGeometry = m_screen->geometry();
    m_currentScreenPixmap = m_screen->grabWindow(0);
    m_currentScreenImage = m_currentScreenPixmap.toImage();
    m_devicePixelRatio = m_currentScreenPixmap.isNull()
                             ? m_screen->devicePixelRatio()
                             : m_currentScreenPixmap.devicePixelRatio();
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
    for (const auto &window : s_windows) {
        if (!window.rect.contains(globalLogicalPos)) {
            continue;
        }

        const QRect localRect = window.rect.intersected(m_screenGeometry).translated(-m_screenGeometry.topLeft());
        if (localRect.isValid() && !localRect.isEmpty()) {
            return localRect;
        }
    }

    return {};
}

void ScreenCapturer::setInputTransparent(bool transparent)
{
    const auto hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        return;
    }

    if (m_inputTransparent == transparent) {
        return;
    }

    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    const LONG_PTR updatedStyle = transparent ? (exStyle | WS_EX_TRANSPARENT) : (exStyle & ~WS_EX_TRANSPARENT);
    if (updatedStyle != exStyle) {
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, updatedStyle);
    }
    m_inputTransparent = transparent;
}

QRect ScreenCapturer::uiaPhysicalRectToLocalLogical(const QRect &globalPhysicalRect) const
{
    return CoordinateConverter::physicalRectToLocalLogical(globalPhysicalRect, m_screen);
}

bool ScreenCapturer::shouldAcceptHoveredRect(const QRect &candidateRect) const
{
    if (!candidateRect.isValid() || candidateRect.isEmpty()) {
        return false;
    }

    if (!m_hoveredRect.isValid() || m_hoveredRect.isEmpty()) {
        return true;
    }

    const QPoint currentCenter = m_hoveredRect.center();
    const QPoint candidateCenter = candidateRect.center();
    const int centerDelta = std::abs(currentCenter.x() - candidateCenter.x())
                            + std::abs(currentCenter.y() - candidateCenter.y());

    const int currentArea = m_hoveredRect.width() * m_hoveredRect.height();
    const int candidateArea = candidateRect.width() * candidateRect.height();
    const int areaDelta = std::abs(candidateArea - currentArea);

    return centerDelta > 6 || areaDelta > (currentArea / 5);
}

int ScreenCapturer::hoverHandleAt(const QPoint &localPos) const
{
    if (!m_hoveredRect.isValid() || m_hoveredRect.isEmpty()) {
        return kHandleNone;
    }

    const QRect outer = inflateRect(m_hoveredRect, kAdjustMargin);
    if (!outer.contains(localPos)) {
        return kHandleNone;
    }

    const bool nearLeft = std::abs(localPos.x() - m_hoveredRect.left()) <= kAdjustMargin;
    const bool nearRight = std::abs(localPos.x() - m_hoveredRect.right()) <= kAdjustMargin;
    const bool nearTop = std::abs(localPos.y() - m_hoveredRect.top()) <= kAdjustMargin;
    const bool nearBottom = std::abs(localPos.y() - m_hoveredRect.bottom()) <= kAdjustMargin;

    if (nearLeft && nearTop) {
        return kHandleTopLeft;
    }
    if (nearRight && nearTop) {
        return kHandleTopRight;
    }
    if (nearLeft && nearBottom) {
        return kHandleBottomLeft;
    }
    if (nearRight && nearBottom) {
        return kHandleBottomRight;
    }
    if (nearLeft) {
        return kHandleLeft;
    }
    if (nearRight) {
        return kHandleRight;
    }
    if (nearTop) {
        return kHandleTop;
    }
    if (nearBottom) {
        return kHandleBottom;
    }
    if (m_hoveredRect.contains(localPos)) {
        return kHandleMove;
    }

    return kHandleNone;
}

void ScreenCapturer::updateAdjustCursor(const QPoint &localPos)
{
    switch (hoverHandleAt(localPos)) {
    case kHandleTopLeft:
    case kHandleBottomRight:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case kHandleTopRight:
    case kHandleBottomLeft:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case kHandleLeft:
    case kHandleRight:
        setCursor(Qt::SizeHorCursor);
        break;
    case kHandleTop:
    case kHandleBottom:
        setCursor(Qt::SizeVerCursor);
        break;
    case kHandleMove:
        setCursor(Qt::SizeAllCursor);
        break;
    default:
        setCursor(Qt::CrossCursor);
        break;
    }
}

QRect ScreenCapturer::adjustedHoveredRect(const QPoint &localPos) const
{
    if (!m_adjustStartRect.isValid() || m_adjustStartRect.isEmpty() || m_activeHandle == kHandleNone) {
        return {};
    }

    QRect adjusted = m_adjustStartRect;
    const QPoint delta = localPos - m_adjustPressPos;

    switch (m_activeHandle) {
    case kHandleMove:
        adjusted.translate(delta);
        break;
    case kHandleLeft:
        adjusted.setLeft(adjusted.left() + delta.x());
        break;
    case kHandleTop:
        adjusted.setTop(adjusted.top() + delta.y());
        break;
    case kHandleRight:
        adjusted.setRight(adjusted.right() + delta.x());
        break;
    case kHandleBottom:
        adjusted.setBottom(adjusted.bottom() + delta.y());
        break;
    case kHandleTopLeft:
        adjusted.setTopLeft(adjusted.topLeft() + delta);
        break;
    case kHandleTopRight:
        adjusted.setTopRight(adjusted.topRight() + delta);
        break;
    case kHandleBottomLeft:
        adjusted.setBottomLeft(adjusted.bottomLeft() + delta);
        break;
    case kHandleBottomRight:
        adjusted.setBottomRight(adjusted.bottomRight() + delta);
        break;
    default:
        break;
    }

    return clampAdjustedRect(adjusted, rect());
}

QRect ScreenCapturer::logicalToDevice(const QRect &rect) const
{
    if (!rect.isValid() || rect.isEmpty()) {
        return {};
    }

    return QRect(qRound(rect.x() * m_devicePixelRatio),
                 qRound(rect.y() * m_devicePixelRatio),
                 std::max(1, qRound(rect.width() * m_devicePixelRatio)),
                 std::max(1, qRound(rect.height() * m_devicePixelRatio)));
}

QPoint ScreenCapturer::logicalToDevice(const QPoint &point) const
{
    return QPoint(qRound(point.x() * m_devicePixelRatio), qRound(point.y() * m_devicePixelRatio));
}

void ScreenCapturer::drawDesktop(QPainter &painter, const QRect &targetClip) const
{
    if (targetClip.isValid() && !targetClip.isEmpty()) {
        painter.save();
        painter.setClipRect(targetClip);
    }

    if (!m_currentScreenPixmap.isNull()) {
        painter.drawPixmap(rect(), m_currentScreenPixmap);
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
    painter.setPen(QPen(QColor(0x20, 0x80, 0xF0), 2, Qt::SolidLine));
    painter.setBrush(QColor(32, 128, 240, 40));
    painter.drawRect(hoveredRect);
    painter.restore();
}

void ScreenCapturer::drawMagnifier(QPainter &painter)
{
    if (!rect().contains(m_mouseLocalPos)) {
        return;
    }

    QImage magnifierImage(kMagnifierSize, kMagnifierSize, QImage::Format_ARGB32_Premultiplied);
    magnifierImage.fill(QColor(24, 24, 24, 230));

    QPainter magnifierPainter(&magnifierImage);
    magnifierPainter.setRenderHint(QPainter::Antialiasing, false);

    const int halfGrid = kMagnifierGrid / 2;
    for (int y = -halfGrid; y <= halfGrid; ++y) {
        for (int x = -halfGrid; x <= halfGrid; ++x) {
            const QPoint samplePoint = m_mouseLocalPos + QPoint(x, y);
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

    const QColor color = sampleColor(m_mouseLocalPos);
    const QString infoText = QStringLiteral("RGB(%1, %2, %3)\nX:%4 Y:%5")
                                 .arg(color.red())
                                 .arg(color.green())
                                 .arg(color.blue())
                                 .arg(m_mouseGlobalPos.x())
                                 .arg(m_mouseGlobalPos.y());

    QFontMetrics metrics(font());
    QRect textRect = metrics.boundingRect(QRect(0, 0, 220, 60), Qt::TextWordWrap, infoText);
    textRect = inflateRect(textRect, kInfoPadding);

    QPoint anchor = m_mouseLocalPos + QPoint(24, 24);
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

QImage ScreenCapturer::extractSelectionImage(const QRect &localLogicalSelection) const
{
    if (!localLogicalSelection.isValid() || localLogicalSelection.isEmpty() || m_currentScreenImage.isNull()) {
        return {};
    }

    const QRect deviceRect = logicalToDevice(localLogicalSelection);
    const QRect bounded = deviceRect.intersected(m_currentScreenImage.rect());
    if (!bounded.isValid() || bounded.isEmpty()) {
        return {};
    }

    QImage output = m_currentScreenImage.copy(bounded);
    output.setDevicePixelRatio(m_devicePixelRatio);
    return output;
}

QColor ScreenCapturer::sampleColor(const QPoint &localLogicalPos) const
{
    if (m_currentScreenImage.isNull()) {
        return Qt::black;
    }

    const QPoint sourcePixel = logicalToDevice(localLogicalPos);
    const QPoint boundedPoint(qBound(0, sourcePixel.x(), m_currentScreenImage.width() - 1),
                              qBound(0, sourcePixel.y(), m_currentScreenImage.height() - 1));
    if (m_currentScreenImage.rect().contains(boundedPoint)) {
        return m_currentScreenImage.pixelColor(boundedPoint);
    }

    return Qt::black;
}

void ScreenCapturer::refreshVisibleWindows()
{
    s_windows.clear();
    WId selfWinId = s_capturers.isEmpty() ? 0 : s_capturers.first()->winId();
    std::pair<QVector<NativeWindowInfo> *, WId> payload{&s_windows, selfWinId};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&payload));

    std::sort(s_windows.begin(), s_windows.end(), [](const NativeWindowInfo &lhs, const NativeWindowInfo &rhs) {
        return lhs.rect.width() * lhs.rect.height() < rhs.rect.width() * rhs.rect.height();
    });
}

void ScreenCapturer::beginAllCaptures()
{
    if (ScreenCapturer::s_isCapturing) {
        closeAllCaptures();
        return;
    }

    refreshVisibleWindows();
    for (ScreenCapturer *capturer : s_capturers) {
        if (capturer) {
            capturer->beginCapture();
        }
    }
}

void ScreenCapturer::closeAllCaptures()
{
    for (ScreenCapturer *capturer : s_capturers) {
        if (capturer) {
            capturer->endCapture();
        }
    }
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
