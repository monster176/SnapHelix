#include "screencapturer.h"
#include "coordinateconverter.h"
#include "uiadetector.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QCursor>
#include <QDebug>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QToolTip>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <iterator>
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
constexpr int kToolbarOuterPadding = 6;
constexpr int kToolbarVerticalPadding = 4;
constexpr int kToolbarHeight = 36;
constexpr int kToolbarButtonSize = 28;
constexpr int kToolbarSpacing = 2;
constexpr int kToolbarMargin = 10;
constexpr int kPaletteMargin = 6;
constexpr int kPaletteSwatchSize = 16;
constexpr int kPaletteSwatchGap = 4;
constexpr int kPaletteInnerPadding = 6;
constexpr int kSelectionHandleRadius = 4;
constexpr int kAnnotationPenWidth = 3;
constexpr int kPinnedFramePadding = 4;
constexpr int kInlineTextBoxWidth = 220;
constexpr int kInlineTextBoxHeight = 34;

struct ToolbarVisualItem
{
    ScreenCapturer::ToolType type;
    bool separator;
};

constexpr ToolbarVisualItem kToolbarItems[] = {
    {ScreenCapturer::Tool_Select, false},
    {ScreenCapturer::Tool_Draw, false},
    {ScreenCapturer::Tool_Text, false},
    {ScreenCapturer::Tool_Arrow, false},
    {ScreenCapturer::Tool_Rectangle, false},
    {ScreenCapturer::Tool_Ellipse, false},
    {ScreenCapturer::Tool_Undo, false},
    {ScreenCapturer::Tool_Redo, false},
    {ScreenCapturer::Tool_COUNT, true},
    {ScreenCapturer::Tool_Close, false},
    {ScreenCapturer::Tool_Pin, false},
    {ScreenCapturer::Tool_Save, false},
    {ScreenCapturer::Tool_Copy, false},
};

constexpr QColor kAnnotationPalette[] = {
    QColor(255, 59, 48),
    QColor(255, 149, 0),
    QColor(255, 204, 0),
    QColor(52, 199, 89),
    QColor(0, 122, 255),
    QColor(88, 86, 214),
    QColor(255, 45, 85),
    QColor(255, 255, 255),
    QColor(28, 28, 30)
};

int toolbarContentWidth()
{
    int width = 0;
    for (const ToolbarVisualItem &item : kToolbarItems) {
        width += item.separator ? 8 : kToolbarButtonSize;
        width += kToolbarSpacing;
    }

    return width > 0 ? width - kToolbarSpacing : 0;
}

int paletteContentWidth()
{
    return kPaletteInnerPadding * 2
           + static_cast<int>(std::size(kAnnotationPalette)) * kPaletteSwatchSize
           + (static_cast<int>(std::size(kAnnotationPalette)) - 1) * kPaletteSwatchGap;
}

bool isAnnotationTool(ScreenCapturer::ToolType tool)
{
    switch (tool) {
    case ScreenCapturer::Tool_Draw:
    case ScreenCapturer::Tool_Text:
    case ScreenCapturer::Tool_Arrow:
    case ScreenCapturer::Tool_Rectangle:
    case ScreenCapturer::Tool_Ellipse:
        return true;
    default:
        return false;
    }
}

bool isNearFullScreenRect(const QRect &candidate, const QRect &bounds)
{
    if (!candidate.isValid() || candidate.isEmpty() || !bounds.isValid() || bounds.isEmpty()) {
        return false;
    }

    return std::abs(candidate.left() - bounds.left()) <= 2
           && std::abs(candidate.top() - bounds.top()) <= 2
           && std::abs(candidate.right() - bounds.right()) <= 2
           && std::abs(candidate.bottom() - bounds.bottom()) <= 2;
}

void drawSelectionHandles(QPainter &painter, const QRect &selection)
{
    const QVector<QPoint> handles = {
        selection.topLeft(),
        selection.topRight(),
        selection.bottomLeft(),
        selection.bottomRight(),
        QPoint(selection.center().x(), selection.top()),
        QPoint(selection.center().x(), selection.bottom()),
        QPoint(selection.left(), selection.center().y()),
        QPoint(selection.right(), selection.center().y())
    };

    painter.save();
    painter.setPen(QPen(QColor(51, 153, 255), 1.5));
    painter.setBrush(Qt::white);
    for (const QPoint &point : handles) {
        painter.drawEllipse(point, kSelectionHandleRadius, kSelectionHandleRadius);
    }
    painter.restore();
}

QString toolDisplayName(ScreenCapturer::ToolType tool)
{
    switch (tool) {
    case ScreenCapturer::Tool_Select:
        return QStringLiteral("选择");
    case ScreenCapturer::Tool_Draw:
        return QStringLiteral("画笔");
    case ScreenCapturer::Tool_Text:
        return QStringLiteral("文字");
    case ScreenCapturer::Tool_Arrow:
        return QStringLiteral("箭头");
    case ScreenCapturer::Tool_Rectangle:
        return QStringLiteral("矩形");
    case ScreenCapturer::Tool_Ellipse:
        return QStringLiteral("椭圆");
    case ScreenCapturer::Tool_Undo:
        return QStringLiteral("撤销");
    case ScreenCapturer::Tool_Redo:
        return QStringLiteral("前进");
    case ScreenCapturer::Tool_Copy:
        return QStringLiteral("复制");
    case ScreenCapturer::Tool_Pin:
        return QStringLiteral("贴图");
    case ScreenCapturer::Tool_Save:
        return QStringLiteral("保存");
    case ScreenCapturer::Tool_Close:
        return QStringLiteral("关闭");
    default:
        return {};
    }
}

class PinnedPreviewWidget final : public QWidget
{
public:
    explicit PinnedPreviewWidget(const QImage &image, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_pixmap(QPixmap::fromImage(image))
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
        setCursor(Qt::OpenHandCursor);
        m_baseImageSize = m_pixmap.size() / m_pixmap.devicePixelRatio();
        resize(outerSizeForScale(m_scaleFactor));
        m_scaleBadgeTimer.setSingleShot(true);
        m_scaleBadgeTimer.setInterval(2000);
        connect(&m_scaleBadgeTimer, &QTimer::timeout, this, [this]() {
            m_showScaleBadge = false;
            update();
        });
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRectF outerRect = rect().adjusted(1, 1, -1, -1);
        const QRectF imageRect(kPinnedFramePadding,
                               kPinnedFramePadding,
                               width() - kPinnedFramePadding * 2,
                               height() - kPinnedFramePadding * 2);
        const QRectF glowRect = outerRect.adjusted(3, 3, -3, -3);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(7, 18, 36, 42));
        painter.drawRoundedRect(outerRect.translated(0, 2), 6, 6);

        for (int i = 0; i < 2; ++i) {
            const QRectF layer = glowRect.adjusted(-i, -i, i, i);
            const int alpha = 34 - i * 10;
            painter.setPen(QPen(QColor(42, 155, 255, alpha), 0.9));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(layer, 6 + i, 6 + i);
        }

        painter.setBrush(QColor(15, 22, 37, 228));
        painter.setPen(QPen(QColor(64, 173, 255, 200), 0.9));
        painter.drawRoundedRect(outerRect, 6, 6);

        QLinearGradient edgeGlow(outerRect.topLeft(), outerRect.topRight());
        edgeGlow.setColorAt(0.0, QColor(120, 210, 255, 0));
        edgeGlow.setColorAt(0.18, QColor(120, 210, 255, 120));
        edgeGlow.setColorAt(0.52, QColor(80, 170, 255, 190));
        edgeGlow.setColorAt(0.82, QColor(120, 210, 255, 96));
        edgeGlow.setColorAt(1.0, QColor(120, 210, 255, 0));
        painter.setPen(QPen(QBrush(edgeGlow), 0.9));
        painter.drawRoundedRect(outerRect.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

        QLinearGradient reflection(outerRect.topLeft(), QPointF(outerRect.left(), outerRect.top() + 26));
        reflection.setColorAt(0.0, QColor(140, 220, 255, 92));
        reflection.setColorAt(0.55, QColor(90, 180, 255, 28));
        reflection.setColorAt(1.0, QColor(90, 180, 255, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(reflection);
        painter.drawRoundedRect(QRectF(outerRect.left() + 8, outerRect.top() + 2, outerRect.width() - 16, 8), 4, 4);

        painter.setBrush(QColor(255, 255, 255, 248));
        painter.setPen(QPen(QColor(70, 176, 255, 132), 0.8));
        painter.drawRoundedRect(imageRect.adjusted(-1, -1, 1, 1), 4, 4);
        painter.drawPixmap(imageRect.toRect(), m_pixmap, m_pixmap.rect());

        if (m_showScaleBadge) {
            const QString scaleText = QStringLiteral("%1%").arg(qRound(m_scaleFactor * 100.0));
            QFont badgeFont(QStringLiteral("Segoe UI"), 9, QFont::DemiBold);
            painter.setFont(badgeFont);
            QFontMetrics metrics(badgeFont);
            const QRect textBounds = metrics.boundingRect(scaleText);
            const QRect badgeRect(10, 8, textBounds.width() + 16, textBounds.height() + 8);

            QLinearGradient badgeGradient(QPointF(badgeRect.topLeft()), QPointF(badgeRect.bottomLeft()));
            badgeGradient.setColorAt(0.0, QColor(18, 52, 96, 228));
            badgeGradient.setColorAt(1.0, QColor(10, 24, 56, 218));
            painter.setPen(QPen(QColor(120, 220, 255, 168), 0.8));
            painter.setBrush(badgeGradient);
            painter.drawRoundedRect(badgeRect, 6, 6);
            painter.setPen(QColor(214, 245, 255));
            painter.drawText(badgeRect, Qt::AlignCenter, scaleText);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }

        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
            QWidget::mouseMoveEvent(event);
            return;
        }

        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragging = false;
            setCursor(Qt::OpenHandCursor);
            event->accept();
            return;
        }

        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            close();
            event->accept();
            return;
        }

        QWidget::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const QPoint pixelDelta = event->pixelDelta();
        const QPoint angleDelta = event->angleDelta();
        if (pixelDelta.y() == 0 && angleDelta.y() == 0) {
            QWidget::wheelEvent(event);
            return;
        }

        const qreal oldScale = m_scaleFactor;
        const int deltaY = pixelDelta.y() != 0 ? pixelDelta.y() : angleDelta.y();
        if (deltaY > 0) {
            m_scaleFactor *= 1.10;
        } else {
            m_scaleFactor /= 1.10;
        }
        m_scaleFactor = qBound(0.1, m_scaleFactor, 10.0);
        if (qFuzzyCompare(oldScale, m_scaleFactor)) {
            event->accept();
            return;
        }

        m_showScaleBadge = true;
        m_scaleBadgeTimer.start();

        const QSize newImageSize(std::max(1, qRound(m_baseImageSize.width() * m_scaleFactor)),
                                 std::max(1, qRound(m_baseImageSize.height() * m_scaleFactor)));
        resize(newImageSize + QSize(kPinnedFramePadding * 2, kPinnedFramePadding * 2));
        update();
        event->accept();
    }

private:
    QSize outerSizeForScale(qreal scale) const
    {
        const QSize scaledSize(std::max(1, qRound(m_baseImageSize.width() * scale)),
                               std::max(1, qRound(m_baseImageSize.height() * scale)));
        return scaledSize + QSize(kPinnedFramePadding * 2, kPinnedFramePadding * 2);
    }

    QPixmap m_pixmap;
    QSize m_baseImageSize;
    QPoint m_dragOffset;
    bool m_dragging = false;
    bool m_showScaleBadge = false;
    qreal m_scaleFactor = 1.0;
    QTimer m_scaleBadgeTimer;
};

void drawToolbarGlyph(QPainter &painter, ScreenCapturer::ToolType tool, const QRect &rect)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0x3a, 0x3a, 0x3a), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    const QRectF r = QRectF(rect).adjusted(6.0, 6.0, -6.0, -6.0);
    const qreal left = r.left();
    const qreal right = r.right();
    const qreal top = r.top();
    const qreal bottom = r.bottom();
    const qreal cx = r.center().x();
    const qreal cy = r.center().y();

    switch (tool) {
    case ScreenCapturer::Tool_Select:
        painter.setBrush(QColor(0x3a, 0x3a, 0x3a));
        painter.drawPolygon(QPolygonF({
            QPointF(left + 1.0, top + 0.5),
            QPointF(left + 1.0, bottom - 1.0),
            QPointF(left + 5.0, bottom - 4.0),
            QPointF(left + 7.0, bottom + 0.2),
            QPointF(left + 9.2, bottom - 0.6),
            QPointF(left + 7.0, bottom - 4.8),
            QPointF(right - 0.8, bottom - 5.0)
        }));
        break;
    case ScreenCapturer::Tool_Draw: {
        QPainterPath path(QPointF(left, bottom - 1.0));
        path.lineTo(QPointF(left + 3.5, cy + 1.0));
        path.lineTo(QPointF(cx - 1.0, cy + 3.0));
        path.lineTo(QPointF(cx + 2.0, top + 4.0));
        path.lineTo(QPointF(right, top + 1.5));
        painter.drawPath(path);
        painter.drawEllipse(QRectF(right - 2.6, top + 0.2, 2.4, 2.4));
        break;
    }
    case ScreenCapturer::Tool_Text: {
        QFont font(QStringLiteral("Segoe UI"), 11, QFont::Normal);
        painter.setFont(font);
        painter.drawText(rect, Qt::AlignCenter, QStringLiteral("T"));
        break;
    }
    case ScreenCapturer::Tool_Arrow: {
        painter.drawLine(QPointF(left + 1.5, bottom - 2.0), QPointF(right - 3.5, top + 3.0));
        painter.drawLine(QPointF(right - 3.5, top + 3.0), QPointF(right - 7.8, top + 3.8));
        painter.drawLine(QPointF(right - 3.5, top + 3.0), QPointF(right - 4.6, top + 7.2));
        break;
    }
    case ScreenCapturer::Tool_Rectangle: {
        const qreal s = 5.2;
        painter.drawRect(QRectF(left, top, s, s));
        painter.drawRect(QRectF(right - s, top, s, s));
        painter.drawRect(QRectF(left, bottom - s, s, s));
        painter.drawRect(QRectF(right - s, bottom - s, s, s));
        break;
    }
    case ScreenCapturer::Tool_Ellipse:
        painter.drawLine(QPointF(left + 2.0, bottom - 2.0), QPointF(cx - 0.5, cy + 0.5));
        painter.drawLine(QPointF(cx - 0.5, cy + 0.5), QPointF(right - 2.0, top + 2.0));
        painter.drawEllipse(QRectF(left + 1.0, bottom - 5.5, 4.5, 4.5));
        break;
    case ScreenCapturer::Tool_Undo: {
        QPainterPath path(QPointF(right - 1.0, bottom - 2.5));
        path.cubicTo(QPointF(cx + 2.5, bottom - 3.0),
                     QPointF(cx + 0.5, top + 1.0),
                     QPointF(left + 4.5, top + 5.0));
        painter.drawPath(path);
        QPainterPath arrowHead(QPointF(left + 4.5, top + 5.0));
        arrowHead.lineTo(QPointF(left + 8.7, top + 2.0));
        arrowHead.moveTo(QPointF(left + 4.5, top + 5.0));
        arrowHead.lineTo(QPointF(left + 9.2, top + 7.5));
        painter.drawPath(arrowHead);
        break;
    }
    case ScreenCapturer::Tool_Redo: {
        QPainterPath path(QPointF(left + 1.0, bottom - 2.5));
        path.cubicTo(QPointF(cx - 2.5, bottom - 3.0),
                     QPointF(cx - 0.5, top + 1.0),
                     QPointF(right - 4.5, top + 5.0));
        painter.drawPath(path);
        QPainterPath arrowHead(QPointF(right - 4.5, top + 5.0));
        arrowHead.lineTo(QPointF(right - 8.7, top + 2.0));
        arrowHead.moveTo(QPointF(right - 4.5, top + 5.0));
        arrowHead.lineTo(QPointF(right - 9.2, top + 7.5));
        painter.drawPath(arrowHead);
        break;
    }
    case ScreenCapturer::Tool_Copy:
        painter.drawRect(QRectF(left + 3.0, top, 9.0, 11.0));
        painter.drawRect(QRectF(left, top + 3.0, 9.0, 11.0));
        break;
    case ScreenCapturer::Tool_Pin: {
        painter.drawLine(QPointF(cx, top + 1.0), QPointF(cx, bottom - 2.0));
        painter.drawLine(QPointF(left + 4.0, top + 4.0), QPointF(right - 4.0, top + 4.0));
        painter.drawLine(QPointF(left + 5.0, top + 4.0), QPointF(cx, cy + 1.0));
        painter.drawLine(QPointF(right - 5.0, top + 4.0), QPointF(cx, cy + 1.0));
        painter.drawLine(QPointF(cx - 3.0, cy + 1.0), QPointF(cx + 3.0, cy + 1.0));
        break;
    }
    case ScreenCapturer::Tool_Save:
        painter.drawRect(QRectF(left, top, 14.0, 14.0));
        painter.drawRect(QRectF(left + 3.0, top + 2.0, 6.0, 3.5));
        painter.drawRect(QRectF(left + 3.0, top + 8.0, 8.0, 4.0));
        break;
    case ScreenCapturer::Tool_Close:
        painter.drawLine(QPointF(left + 1.0, top + 1.0), QPointF(right - 1.0, bottom - 1.0));
        painter.drawLine(QPointF(right - 1.0, top + 1.0), QPointF(left + 1.0, bottom - 1.0));
        break;
    case ScreenCapturer::Tool_COUNT:
        break;
    }

    painter.restore();
}

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
    const bool hasFinalSelection = m_hasSelection && !m_dragging && selection.isValid() && !selection.isEmpty();
    const bool shouldDrawToolbar = m_showToolbar || hasFinalSelection;
    if (selection.isValid() && !selection.isEmpty()) {
        drawSelectionOverlay(painter, selection);
        if (m_selectedTool == Tool_Select
            && isNearFullScreenRect(selection, rect())
            && m_hoveredRect.isValid()
            && !m_hoveredRect.isEmpty()
            && m_hoveredRect != selection) {
            drawHoverOverlay(painter, m_hoveredRect);
        }
        drawAnnotations(painter, selection);
        if (shouldDrawToolbar) {
            drawToolbar(painter, selection);
            drawColorPalette(painter, selection);
        }
    } else if (m_hoveredRect.isValid() && !m_hoveredRect.isEmpty()) {
        drawHoverOverlay(painter, m_hoveredRect);
        if (shouldDrawToolbar) {
            drawToolbar(painter, m_hoveredRect);
            drawColorPalette(painter, m_hoveredRect);
        }
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
    if (m_textEditor) {
        if (event->key() == Qt::Key_Escape) {
            cancelTextEditing();
            return;
        }
        QWidget::keyPressEvent(event);
        return;
    }

    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_C) {
        const QColor color = sampleColor(m_mouseLocalPos);
        qApp->clipboard()->setText(color.name(QColor::HexRgb).toUpper());
        showCopyToast(QStringLiteral("复制成功"));
        return;
    }

    switch (event->key()) {
    case Qt::Key_Escape:
        closeAllCaptures();
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

    if (m_textEditor && !m_textEditor->geometry().contains(m_mouseLocalPos)) {
        commitTextEditing();
    }

    if (event->button() == Qt::RightButton) {
        closeAllCaptures();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    // Check if clicked on toolbar
    ToolType clickedTool = getToolAt(m_mouseLocalPos);
    if (clickedTool != Tool_COUNT) {
        m_ignoreNextMouseRelease = true;
        handleToolClick(clickedTool);
        return;
    }

    const QColor clickedColor = getColorAt(m_mouseLocalPos);
    if (clickedColor.isValid()) {
        m_ignoreNextMouseRelease = true;
        m_annotationColor = clickedColor;
        update();
        return;
    }

    if (beginAnnotationInteraction(m_mouseLocalPos)) {
        if (!m_isAnnotating) {
            m_ignoreNextMouseRelease = true;
        }
        update();
        return;
    }

    const QRect currentRect = currentSelection();
    const bool shouldProbeSubRect = m_selectedTool == Tool_Select
                                    && currentRect.isValid()
                                    && !currentRect.isEmpty()
                                    && isNearFullScreenRect(currentRect, rect())
                                    && m_hoveredRect.isValid()
                                    && !m_hoveredRect.isEmpty()
                                    && m_hoveredRect != currentRect;
    m_clickSelectionCandidate = shouldProbeSubRect
                                    ? m_hoveredRect
                                    : (currentRect.isValid() && !currentRect.isEmpty() ? currentRect : m_hoveredRect);
    const QRect interactiveRect = shouldProbeSubRect ? m_hoveredRect : selectionBounds();
    const int handle = interactiveRect.isValid() && !interactiveRect.isEmpty() ? hoverHandleAt(m_mouseLocalPos) : kHandleNone;

    if (handle != kHandleNone) {
        m_isCaptured = true;
        m_isResizing = true;
        m_activeHandle = handle;
        m_adjustPressPos = m_mouseLocalPos;
        m_adjustStartRect = interactiveRect;
        m_isProbing = false;
        setInputTransparent(false);
        updateAdjustCursor(m_mouseLocalPos);
        update();
        return;
    }

    m_isCaptured = false;
    m_isResizing = false;
    m_activeHandle = kHandleNone;
    m_showToolbar = false;
    m_annotations.clear();
    m_redoAnnotations.clear();
    m_annotationPreviewRect = {};
    m_annotationPreview.points.clear();
    m_dragging = true;
    m_hasSelection = true;
    m_dragStart = m_mouseLocalPos;
    m_dragCurrent = m_dragStart;
    setCursor(Qt::CrossCursor);
    update();
}

void ScreenCapturer::mouseMoveEvent(QMouseEvent *event)
{
    m_mouseLocalPos = event->position().toPoint();
    m_mouseGlobalPos = event->globalPosition().toPoint();

    const ToolType hoveredTool = getToolAt(m_mouseLocalPos);
    if (hoveredTool != m_hoveredToolbarTool) {
        m_hoveredToolbarTool = hoveredTool;
        if (hoveredTool != Tool_COUNT) {
            const QString tooltip = toolDisplayName(hoveredTool);
            if (!tooltip.isEmpty()) {
                QToolTip::showText(event->globalPosition().toPoint(), tooltip, this);
            }
        } else {
            QToolTip::hideText();
        }
    }

    if (hoveredTool != Tool_COUNT) {
        setCursor(Qt::ArrowCursor);
        update();
        return;
    }

    if (m_isResizing) {
        const QRect adjustedRect = adjustedHoveredRect(m_mouseLocalPos);
        if (adjustedRect.isValid() && !adjustedRect.isEmpty()) {
            m_hoveredRect = adjustedRect;
            m_dragStart = adjustedRect.topLeft();
            m_dragCurrent = adjustedRect.bottomRight();
            m_hasSelection = true;
            m_isCaptured = true;
        }
        updateAdjustCursor(m_mouseLocalPos);
        update();
        return;
    }

    if (m_isAnnotating) {
        updateAnnotationInteraction(m_mouseLocalPos);
        update();
        return;
    }

    const QRect currentRect = currentSelection();
    const bool shouldProbeSubRect = m_selectedTool == Tool_Select
                                    && currentRect.isValid()
                                    && !currentRect.isEmpty()
                                    && isNearFullScreenRect(currentRect, rect());
    const QRect interactiveRect = shouldProbeSubRect && m_hoveredRect.isValid() && !m_hoveredRect.isEmpty()
                                      ? m_hoveredRect
                                      : selectionBounds();
    if (interactiveRect.isValid() && !interactiveRect.isEmpty()) {
        const int hoveredHandle = hoverHandleAt(m_mouseLocalPos);
        if (!shouldProbeSubRect && (m_isCaptured || hoveredHandle != kHandleNone)) {
            updateAdjustCursor(m_mouseLocalPos);
            update();
            return;
        }

        if (shouldProbeSubRect && hoveredHandle != kHandleNone) {
            updateAdjustCursor(m_mouseLocalPos);
            update();
        }
    }

    if (m_dragging) {
        m_dragCurrent = m_mouseLocalPos;
        update();
        return;
    }

    if (m_showToolbar && !shouldProbeSubRect) {
        setCursor(Qt::ArrowCursor);
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
    QtConcurrent::run([this, probePos, handleValue, shouldProbeSubRect]() {
        const QRect rect = UIADetector::instance().getElementRectAt(
            probePos, reinterpret_cast<void *>(handleValue));

        QMetaObject::invokeMethod(this,
                                  [this, rect, probePos, shouldProbeSubRect]() {
                                      setInputTransparent(false);

                                      if (m_isCaptured && !shouldProbeSubRect) {
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

    if (m_ignoreNextMouseRelease) {
        m_ignoreNextMouseRelease = false;
        return;
    }

    if (m_isAnnotating) {
        finishAnnotationInteraction(m_mouseLocalPos);
        update();
        return;
    }

    if (m_isCaptured && m_isResizing) {
        const QRect adjustedRect = adjustedHoveredRect(m_mouseLocalPos);
        if (adjustedRect.isValid() && !adjustedRect.isEmpty()) {
            m_hoveredRect = adjustedRect;
            m_dragStart = adjustedRect.topLeft();
            m_dragCurrent = adjustedRect.bottomRight();
            m_hasSelection = true;
        }
        m_isResizing = false;
        m_activeHandle = kHandleNone;
        m_adjustPressPos = {};
        m_adjustStartRect = {};
        m_clickSelectionCandidate = {};
        m_showToolbar = true;
        setCursor(Qt::ArrowCursor);
        updateAdjustCursor(m_mouseLocalPos);
        update();
        return;
    }

    m_isCaptured = false;
    m_isResizing = false;

    m_dragCurrent = event->position().toPoint();
    m_dragging = false;

    const QRect manualSelection = currentSelection();
    if ((!manualSelection.isValid() || manualSelection.width() < kSelectionMinimum
         || manualSelection.height() < kSelectionMinimum)
        && m_clickSelectionCandidate.isValid()) {
        m_dragStart = m_clickSelectionCandidate.topLeft();
        m_dragCurrent = m_clickSelectionCandidate.bottomRight();
        m_hoveredRect = m_clickSelectionCandidate;
        m_hasSelection = true;
        m_isCaptured = true;
    } else if (manualSelection.isValid() && manualSelection.width() >= kSelectionMinimum
               && manualSelection.height() >= kSelectionMinimum) {
        m_hoveredRect = manualSelection;
        m_isCaptured = true;
    }
    m_clickSelectionCandidate = {};

    // Show toolbar when selection is completed
    if (m_hasSelection || m_isCaptured) {
        m_showToolbar = true;
        setCursor(Qt::ArrowCursor);
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
    m_clickSelectionCandidate = {};
    m_annotationStart = {};
    m_annotationPreviewRect = {};
    m_isAnnotating = false;
    m_ignoreNextMouseRelease = false;
    m_annotations.clear();
    m_redoAnnotations.clear();
    m_isCaptured = false;
    m_isResizing = false;
    m_activeHandle = kHandleNone;
    m_showToolbar = false;
    m_selectedTool = Tool_Select;
    m_hoveredToolbarTool = Tool_COUNT;
    m_colorButtons.clear();
    cancelTextEditing();
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
    cancelTextEditing();
    QToolTip::hideText();
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
    m_clickSelectionCandidate = {};
    m_annotationStart = {};
    m_annotationPreviewRect = {};
    m_isAnnotating = false;
    m_ignoreNextMouseRelease = false;
    m_annotations.clear();
    m_redoAnnotations.clear();
    m_isCaptured = false;
    m_isResizing = false;
    m_activeHandle = kHandleNone;
    m_showToolbar = false;
    m_colorButtons.clear();
    m_selectedTool = Tool_Select;
    m_hoveredToolbarTool = Tool_COUNT;
    setInputTransparent(false);
    ScreenCapturer::s_isCapturing = false;
}

void ScreenCapturer::acceptCapture()
{
    commitTextEditing();
    const QImage image = renderSelectionImage();
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
    default:
        setCursor(m_selectedTool == Tool_Select ? Qt::ArrowCursor : Qt::CrossCursor);
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
    drawSelectionHandles(painter, selection);

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
    drawSelectionHandles(painter, hoveredRect);
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
    const QString hexText = color.name(QColor::HexRgb).toUpper();
    const QString infoText = QStringLiteral("HEX:%1\nRGB(%2, %3, %4)\nX:%5 Y:%6")
                                 .arg(hexText)
                                 .arg(color.red())
                                 .arg(color.green())
                                 .arg(color.blue())
                                 .arg(m_mouseGlobalPos.x())
                                 .arg(m_mouseGlobalPos.y());

    QFontMetrics metrics(font());
    QRect textRect = metrics.boundingRect(QRect(0, 0, 220, 80), Qt::TextWordWrap, infoText);
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

void ScreenCapturer::drawToolbar(QPainter &painter, const QRect &selection)
{
    if (!m_showToolbar || !selection.isValid() || selection.isEmpty()) {
        m_toolButtons.clear();
        return;
    }

    const int contentWidth = toolbarContentWidth();
    const int toolbarWidth = contentWidth + kToolbarOuterPadding * 2;
    int toolbarX = selection.right() - toolbarWidth + 1;
    int toolbarY = selection.bottom() + kToolbarMargin;

    if (toolbarY + kToolbarHeight > height() - kToolbarMargin) {
        toolbarY = selection.top() - kToolbarHeight - kToolbarMargin;
    }
    if (toolbarY < kToolbarMargin) {
        toolbarY = qMin(height() - kToolbarHeight - kToolbarMargin, selection.bottom() + kToolbarMargin);
    }

    toolbarX = qBound(kToolbarMargin, toolbarX, width() - toolbarWidth - kToolbarMargin);
    const QRect toolbarRect(toolbarX, toolbarY, toolbarWidth, kToolbarHeight);

    painter.save();
    painter.setPen(QPen(QColor(0, 0, 0, 28), 1));
    painter.setBrush(QColor(255, 255, 255, 246));
    painter.drawRoundedRect(toolbarRect, 6, 6);
    painter.restore();

    m_toolButtons.clear();
    int x = toolbarRect.left() + kToolbarOuterPadding;
    const int buttonY = toolbarRect.top() + kToolbarVerticalPadding;
    for (const ToolbarVisualItem &item : kToolbarItems) {
        if (item.separator) {
            const int separatorX = x + 3;
            painter.save();
            painter.setPen(QPen(QColor(0, 0, 0, 24), 1));
            painter.drawLine(separatorX, toolbarRect.top() + 7, separatorX, toolbarRect.bottom() - 7);
            painter.restore();
            x += 8 + kToolbarSpacing;
            continue;
        }

        const QRect buttonRect(x, buttonY, kToolbarButtonSize, kToolbarButtonSize);
        painter.save();
        if (item.type == m_selectedTool) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 20));
            painter.drawRoundedRect(buttonRect, 4, 4);
        }
        painter.restore();

        drawToolbarGlyph(painter, item.type, buttonRect);
        m_toolButtons.push_back({buttonRect, item.type});
        x += kToolbarButtonSize + kToolbarSpacing;
    }
}

void ScreenCapturer::drawColorPalette(QPainter &painter, const QRect &selection)
{
    m_colorButtons.clear();
    if (!m_showToolbar || !selection.isValid() || selection.isEmpty() || !isAnnotationTool(m_selectedTool)) {
        return;
    }

    const int paletteWidth = paletteContentWidth();
    const int paletteHeight = kPaletteInnerPadding * 2 + kPaletteSwatchSize;
    int paletteX = selection.left();
    int paletteY = selection.bottom() + kToolbarHeight + kToolbarMargin + kPaletteMargin;

    if (paletteY + paletteHeight > height() - kToolbarMargin) {
        paletteY = selection.top() - kToolbarHeight - kToolbarMargin - kPaletteMargin - paletteHeight;
    }
    if (paletteY < kToolbarMargin) {
        paletteY = qBound(kToolbarMargin, selection.bottom() + kToolbarMargin, height() - paletteHeight - kToolbarMargin);
    }

    paletteX = qBound(kToolbarMargin, paletteX, width() - paletteWidth - kToolbarMargin);
    const QRect paletteRect(paletteX, paletteY, paletteWidth, paletteHeight);

    painter.save();
    painter.setPen(QPen(QColor(0, 0, 0, 28), 1));
    painter.setBrush(QColor(255, 255, 255, 246));
    painter.drawRoundedRect(paletteRect, 6, 6);
    painter.restore();

    int x = paletteRect.left() + kPaletteInnerPadding;
    const int y = paletteRect.top() + kPaletteInnerPadding;
    for (const QColor &color : kAnnotationPalette) {
        const QRect swatchRect(x, y, kPaletteSwatchSize, kPaletteSwatchSize);
        painter.save();
        painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
        painter.setBrush(color);
        painter.drawRect(swatchRect);
        if (color == m_annotationColor) {
            painter.setPen(QPen(QColor(0, 122, 255), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(swatchRect.adjusted(-2, -2, 2, 2));
        }
        painter.restore();

        m_colorButtons.push_back({swatchRect, color});
        x += kPaletteSwatchSize + kPaletteSwatchGap;
    }
}

ScreenCapturer::ToolType ScreenCapturer::getToolAt(const QPoint &pos) const
{
    for (const auto &button : m_toolButtons) {
        if (button.rect.contains(pos)) {
            return button.type;
        }
    }
    return Tool_COUNT;
}

QColor ScreenCapturer::getColorAt(const QPoint &pos) const
{
    for (const auto &button : m_colorButtons) {
        if (button.rect.contains(pos)) {
            return button.color;
        }
    }

    return {};
}

void ScreenCapturer::handleToolClick(ToolType tool)
{
    if (tool != Tool_Text) {
        commitTextEditing();
    }

    switch (tool) {
    case Tool_Select:
        m_selectedTool = Tool_Select;
        setCursor(Qt::ArrowCursor);
        break;
    case Tool_Draw:
        m_selectedTool = Tool_Draw;
        setCursor(Qt::CrossCursor);
        break;
    case Tool_Text:
        m_selectedTool = Tool_Text;
        setCursor(Qt::IBeamCursor);
        break;
    case Tool_Arrow:
        m_selectedTool = Tool_Arrow;
        setCursor(Qt::CrossCursor);
        break;
    case Tool_Rectangle:
        m_selectedTool = Tool_Rectangle;
        setCursor(Qt::CrossCursor);
        break;
    case Tool_Ellipse:
        m_selectedTool = Tool_Ellipse;
        setCursor(Qt::CrossCursor);
        break;
    case Tool_Undo:
        if (!m_annotations.isEmpty()) {
            m_redoAnnotations.push_back(m_annotations.takeLast());
        }
        break;
    case Tool_Redo:
        if (!m_redoAnnotations.isEmpty()) {
            m_annotations.push_back(m_redoAnnotations.takeLast());
        }
        break;
    case Tool_Copy:
        acceptCapture();
        break;
    case Tool_Pin:
        pinSelection();
        break;
    case Tool_Save:
        saveSelectionToFile();
        break;
    case Tool_Close:
        closeAllCaptures();
        break;
    default:
        break;
    }
    update();
}

bool ScreenCapturer::isPointInSelection(const QPoint &pos) const
{
    const QRect selection = selectionBounds();
    return selection.isValid() && !selection.isEmpty() && selection.contains(pos);
}

QRect ScreenCapturer::selectionBounds() const
{
    const QRect selection = currentSelection();
    if (selection.isValid() && !selection.isEmpty()) {
        return selection;
    }

    return m_hoveredRect;
}

QPoint ScreenCapturer::mapToSelection(const QPoint &pos) const
{
    return pos;
}

QRect ScreenCapturer::mapRectToSelection(const QRect &rect) const
{
    return rect;
}

bool ScreenCapturer::beginAnnotationInteraction(const QPoint &localPos)
{
    if (!m_showToolbar || !isAnnotationTool(m_selectedTool) || !isPointInSelection(localPos)) {
        return false;
    }

    if (m_selectedTool == Tool_Text) {
        beginTextEditing(localPos);
        setCursor(Qt::IBeamCursor);
        return true;
    }

    if (m_selectedTool == Tool_ColorPicker) {
        m_annotationColor = sampleColor(localPos);
        setCursor(Qt::ArrowCursor);
        return true;
    }

    if (m_selectedTool == Tool_Text) {
        bool ok = false;
        const QString text = QInputDialog::getText(this,
                                                   QStringLiteral("添加文字"),
                                                   QStringLiteral("请输入文字"),
                                                   QLineEdit::Normal,
                                                   QString(),
                                                   &ok);
        if (ok && !text.trimmed().isEmpty()) {
            clearRedoStack();
            Annotation annotation;
            annotation.kind = Annotation::Text;
            annotation.color = m_annotationColor;
            annotation.text = text;
            annotation.textPos = mapToSelection(localPos);
            m_annotations.push_back(annotation);
        }
        setCursor(Qt::IBeamCursor);
        return true;
    }

    m_isAnnotating = true;
    m_annotationStart = localPos;
    m_annotationPreviewRect = QRect(localPos, localPos).normalized();
    m_annotationPreview = {};
    m_annotationPreview.color = m_annotationColor;

    if (m_selectedTool == Tool_Draw) {
        m_annotationPreview.kind = Annotation::Path;
        m_annotationPreview.points = {QPointF(mapToSelection(localPos))};
    } else if (m_selectedTool == Tool_Arrow) {
        m_annotationPreview.kind = Annotation::Arrow;
        const QPointF anchor = QPointF(mapToSelection(localPos));
        m_annotationPreview.points = {anchor, anchor};
    } else if (m_selectedTool == Tool_Rectangle) {
        m_annotationPreview.kind = Annotation::Rectangle;
        m_annotationPreview.rect = mapRectToSelection(m_annotationPreviewRect);
    } else if (m_selectedTool == Tool_Ellipse) {
        m_annotationPreview.kind = Annotation::Ellipse;
        m_annotationPreview.rect = mapRectToSelection(m_annotationPreviewRect);
    }

    return true;
}

void ScreenCapturer::updateAnnotationInteraction(const QPoint &localPos)
{
    if (!m_isAnnotating) {
        return;
    }

    const QRect selection = selectionBounds();
    const QPoint boundedPos(qBound(selection.left(), localPos.x(), selection.right()),
                            qBound(selection.top(), localPos.y(), selection.bottom()));

    if (m_selectedTool == Tool_Draw) {
        m_annotationPreview.points.push_back(QPointF(mapToSelection(boundedPos)));
        return;
    }

    if (m_selectedTool == Tool_Arrow) {
        if (m_annotationPreview.points.isEmpty()) {
            m_annotationPreview.points = {QPointF(mapToSelection(m_annotationStart)), QPointF(mapToSelection(boundedPos))};
        } else if (m_annotationPreview.points.size() == 1) {
            m_annotationPreview.points.push_back(QPointF(mapToSelection(boundedPos)));
        } else {
            m_annotationPreview.points[1] = QPointF(mapToSelection(boundedPos));
        }
        return;
    }

    m_annotationPreviewRect = QRect(m_annotationStart, boundedPos).normalized();
    m_annotationPreview.rect = mapRectToSelection(m_annotationPreviewRect);
}

void ScreenCapturer::finishAnnotationInteraction(const QPoint &localPos)
{
    if (!m_isAnnotating) {
        return;
    }

    updateAnnotationInteraction(localPos);
    m_isAnnotating = false;

    bool shouldCommit = false;
    if (m_annotationPreview.kind == Annotation::Path) {
        shouldCommit = m_annotationPreview.points.size() > 1;
    } else if (m_annotationPreview.kind == Annotation::Arrow) {
        shouldCommit = m_annotationPreview.points.size() >= 2
                       && QLineF(m_annotationPreview.points[0], m_annotationPreview.points[1]).length() >= kSelectionMinimum;
    } else {
        shouldCommit = m_annotationPreview.rect.isValid()
                       && m_annotationPreview.rect.width() >= kSelectionMinimum
                       && m_annotationPreview.rect.height() >= kSelectionMinimum;
    }

    if (shouldCommit) {
        clearRedoStack();
        m_annotations.push_back(m_annotationPreview);
    }

    m_annotationPreview = {};
    m_annotationPreviewRect = {};
}

void ScreenCapturer::clearRedoStack()
{
    m_redoAnnotations.clear();
}

void ScreenCapturer::drawAnnotations(QPainter &painter, const QRect &selection) const
{
    if (!selection.isValid() || selection.isEmpty()) {
        return;
    }

    for (const Annotation &annotation : m_annotations) {
        drawAnnotation(painter, selection, annotation);
    }

    if (m_isAnnotating) {
        drawAnnotation(painter, selection, m_annotationPreview);
    }
}

void ScreenCapturer::drawAnnotation(QPainter &painter, const QRect &selection, const Annotation &annotation) const
{
    painter.save();
    painter.setClipRect(selection.adjusted(1, 1, -1, -1));
    painter.setPen(QPen(annotation.color, kAnnotationPenWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    switch (annotation.kind) {
    case Annotation::Path: {
        if (annotation.points.isEmpty()) {
            break;
        }
        QPainterPath path(annotation.points.first());
        for (int i = 1; i < annotation.points.size(); ++i) {
            path.lineTo(annotation.points[i]);
        }
        painter.drawPath(path);
        break;
    }
    case Annotation::Arrow: {
        if (annotation.points.size() < 2) {
            break;
        }

        const QPointF start = annotation.points[0];
        const QPointF end = annotation.points[1];
        painter.drawLine(start, end);

        const QLineF line(start, end);
        if (line.length() > 0.0) {
            const qreal arrowSize = 12.0;
            const qreal angle = std::atan2(-line.dy(), line.dx());
            const QPointF arrowP1 = end - QPointF(std::cos(angle + M_PI / 6.0) * arrowSize,
                                                  -std::sin(angle + M_PI / 6.0) * arrowSize);
            const QPointF arrowP2 = end - QPointF(std::cos(angle - M_PI / 6.0) * arrowSize,
                                                  -std::sin(angle - M_PI / 6.0) * arrowSize);
            painter.setBrush(annotation.color);
            painter.drawPolygon(QPolygonF({end, arrowP1, arrowP2}));
        }
        break;
    }
    case Annotation::Rectangle:
        painter.drawRect(annotation.rect);
        break;
    case Annotation::Ellipse:
        painter.drawEllipse(annotation.rect);
        break;
    case Annotation::Text: {
        painter.setPen(annotation.color);
        QFont textFont(QStringLiteral("Segoe UI"), 16, QFont::Bold);
        painter.setFont(textFont);
        painter.drawText(annotation.textPos, annotation.text);
        break;
    }
    }

    painter.restore();
}

void ScreenCapturer::copySelectionToClipboard()
{
    commitTextEditing();
    const QImage image = renderSelectionImage();
    if (!image.isNull()) {
        qApp->clipboard()->setImage(image);
    }
}

bool ScreenCapturer::saveSelectionToFile()
{
    commitTextEditing();
    const QImage image = renderSelectionImage(false);
    if (image.isNull()) {
        return false;
    }

    const QString defaultPath = QDir::home().filePath(QStringLiteral("Pictures/snipaste_capture.png"));
    const QString filePath = QFileDialog::getSaveFileName(this,
                                                          QStringLiteral("保存截图"),
                                                          defaultPath,
                                                          QStringLiteral("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg);;BMP Image (*.bmp)"));
    if (filePath.isEmpty()) {
        return false;
    }

    return image.save(filePath);
}

void ScreenCapturer::beginTextEditing(const QPoint &localPos)
{
    cancelTextEditing();

    const QRect selection = selectionBounds();
    if (!selection.isValid() || selection.isEmpty()) {
        return;
    }

    QRect editorRect(localPos, QSize(kInlineTextBoxWidth, kInlineTextBoxHeight));
    if (editorRect.right() > selection.right() - 8) {
        editorRect.moveRight(selection.right() - 8);
    }
    if (editorRect.bottom() > selection.bottom() - 8) {
        editorRect.moveBottom(selection.bottom() - 8);
    }
    if (editorRect.left() < selection.left() + 8) {
        editorRect.moveLeft(selection.left() + 8);
    }
    if (editorRect.top() < selection.top() + 8) {
        editorRect.moveTop(selection.top() + 8);
    }

    auto *editor = new QLineEdit(this);
    m_textEditor = editor;
    m_textEditAnchor = mapToSelection(editorRect.topLeft());

    QFont textFont(QStringLiteral("Segoe UI"), 16, QFont::Bold);
    editor->setFont(textFont);
    editor->setGeometry(editorRect);
    editor->setFrame(false);
    editor->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    editor->setStyleSheet(QStringLiteral(
                              "QLineEdit {"
                              " background: rgba(255,255,255,235);"
                              " color: %1;"
                              " border: 2px solid %1;"
                              " border-radius: 4px;"
                              " padding: 2px 8px;"
                              " selection-background-color: rgba(0,122,255,110);"
                              "}")
                              .arg(m_annotationColor.name()));
    connect(editor, &QLineEdit::returnPressed, this, [this, editor]() {
        if (m_textEditor == editor) {
            commitTextEditing();
        }
    });
    connect(editor, &QLineEdit::editingFinished, this, [this, editor]() {
        if (m_textEditor == editor) {
            commitTextEditing();
        }
    });

    editor->show();
    editor->raise();
    editor->setFocus(Qt::MouseFocusReason);
}

void ScreenCapturer::commitTextEditing()
{
    if (!m_textEditor) {
        return;
    }

    QPointer<QLineEdit> editor = m_textEditor;
    const QString text = editor->text().trimmed();
    const QFont editorFont = editor->font();
    m_textEditor = nullptr;

    if (!text.isEmpty()) {
        clearRedoStack();
        Annotation annotation;
        annotation.kind = Annotation::Text;
        annotation.color = m_annotationColor;
        annotation.text = text;
        annotation.textPos = m_textEditAnchor + QPoint(0, QFontMetrics(editorFont).ascent() + 2);
        m_annotations.push_back(annotation);
    }

    if (editor) {
        editor->deleteLater();
    }

    setFocus(Qt::OtherFocusReason);
    update();
}

void ScreenCapturer::cancelTextEditing()
{
    if (!m_textEditor) {
        return;
    }

    QPointer<QLineEdit> editor = m_textEditor;
    m_textEditor = nullptr;
    if (editor) {
        editor->deleteLater();
    }
    setFocus(Qt::OtherFocusReason);
    update();
}

void ScreenCapturer::showCopyToast(const QString &text)
{
    if (!m_copyToastLabel) {
        auto *label = new QLabel(this);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setStyleSheet(QStringLiteral(
            "QLabel {"
            " background: rgba(24, 24, 28, 228);"
            " color: white;"
            " border: 1px solid rgba(255,255,255,36);"
            " border-radius: 10px;"
            " padding: 8px 14px;"
            " font: 10pt \"Segoe UI\";"
            "}"));
        m_copyToastLabel = label;
    }

    if (!m_copyToastTimer) {
        m_copyToastTimer = new QTimer(this);
        m_copyToastTimer->setSingleShot(true);
        connect(m_copyToastTimer, &QTimer::timeout, this, [this]() {
            if (m_copyToastLabel) {
                m_copyToastLabel->hide();
            }
        });
    }

    m_copyToastLabel->setText(text);
    m_copyToastLabel->adjustSize();
    const int margin = 18;
    const QSize labelSize = m_copyToastLabel->size();
    const QPoint anchor = m_mouseLocalPos + QPoint(20, -labelSize.height() - 20);
    QPoint topLeft = anchor;
    if (topLeft.x() + labelSize.width() > width() - margin) {
        topLeft.setX(width() - labelSize.width() - margin);
    }
    if (topLeft.x() < margin) {
        topLeft.setX(margin);
    }
    if (topLeft.y() < margin) {
        topLeft.setY(m_mouseLocalPos.y() + 24);
    }
    if (topLeft.y() + labelSize.height() > height() - margin) {
        topLeft.setY(height() - labelSize.height() - margin);
    }

    m_copyToastLabel->move(topLeft);
    m_copyToastLabel->raise();
    m_copyToastLabel->show();
    m_copyToastTimer->start(2000);
}

void ScreenCapturer::pinSelection()
{
    commitTextEditing();
    const QImage image = renderSelectionImage(false);
    if (image.isNull()) {
        return;
    }

    QRect selection = currentSelection();
    if ((!selection.isValid() || selection.width() < kSelectionMinimum || selection.height() < kSelectionMinimum)
        && m_hoveredRect.isValid()) {
        selection = m_hoveredRect;
    }

    auto *window = new PinnedPreviewWidget(image);
    if (selection.isValid() && !selection.isEmpty()) {
        const QPoint globalTopLeft = m_screenGeometry.topLeft() + selection.topLeft() - QPoint(kPinnedFramePadding, kPinnedFramePadding);
        window->move(globalTopLeft);
    } else {
        window->move(QCursor::pos());
    }
    window->show();
    m_pinnedWindows.push_back(window);
}

QImage ScreenCapturer::renderSelectionImage(bool includeDevicePixelRatio) const
{
    QRect selection = currentSelection();
    if ((!selection.isValid() || selection.width() < kSelectionMinimum || selection.height() < kSelectionMinimum)
        && m_hoveredRect.isValid()) {
        selection = m_hoveredRect;
    }

    if (!selection.isValid() || selection.isEmpty()) {
        return {};
    }

    QImage output = extractSelectionImage(selection);
    if (output.isNull()) {
        return {};
    }

    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(-selection.topLeft());
    for (const Annotation &annotation : m_annotations) {
        drawAnnotation(painter, selection, annotation);
    }
    painter.end();

    if (!includeDevicePixelRatio) {
        output.setDevicePixelRatio(1.0);
    }

    return output;
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
