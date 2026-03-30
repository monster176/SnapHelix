#pragma once

#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QPixmap>
#include <QRect>
#include <QVector>
#include <QWidget>

class QColor;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QScreen;
class QShowEvent;

class ScreenCapturer final : public QWidget, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    struct NativeWindowInfo
    {
        QRect rect;
    };

    explicit ScreenCapturer(QScreen *screen, bool controllerInstance, QWidget *parent = nullptr);
    ~ScreenCapturer() override;

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

    static QList<ScreenCapturer *> createCapturers();
    static bool isCandidateWindow(void *handle, WId selfWinId);
    static QRect nativeWindowRectToLogical(void *handle);
    static bool s_isCapturing;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void registerHotkey();
    void unregisterHotkey();

    void beginCapture();
    void endCapture();
    void acceptCapture();

    void refreshScreenSnapshot();
    QRect currentSelection() const;
    QRect normalizedSelection(const QPoint &start, const QPoint &end) const;
    QRect hoveredWindowRect(const QPoint &globalLogicalPos) const;
    void setInputTransparent(bool transparent);
    QRect uiaPhysicalRectToLocalLogical(const QRect &globalPhysicalRect) const;
    bool shouldAcceptHoveredRect(const QRect &candidateRect) const;
    QRect logicalToDevice(const QRect &rect) const;
    QPoint logicalToDevice(const QPoint &point) const;
    int hoverHandleAt(const QPoint &localPos) const;
    void updateAdjustCursor(const QPoint &localPos);
    QRect adjustedHoveredRect(const QPoint &localPos) const;

    void drawDesktop(QPainter &painter, const QRect &targetClip = QRect()) const;
    void drawSelectionOverlay(QPainter &painter, const QRect &selection);
    void drawHoverOverlay(QPainter &painter, const QRect &hoveredRect);
    void drawMagnifier(QPainter &painter);

    QImage extractSelectionImage(const QRect &localLogicalSelection) const;
    QColor sampleColor(const QPoint &localLogicalPos) const;

    static void refreshVisibleWindows();
    static void beginAllCaptures();
    static void closeAllCaptures();

    QPointer<QScreen> m_screen;
    QRect m_screenGeometry;
    QPixmap m_currentScreenPixmap;
    QImage m_currentScreenImage;
    qreal m_devicePixelRatio = 1.0;

    QPoint m_dragStart;
    QPoint m_dragCurrent;
    QPoint m_mouseLocalPos;
    QPoint m_mouseGlobalPos;
    QPoint m_lastProbePos;
    QPoint m_adjustPressPos;

    QRect m_hoveredRect;
    QRect m_adjustStartRect;
    QElapsedTimer m_probeTimer;
    bool m_captureActive = false;
    bool m_dragging = false;
    bool m_hasSelection = false;
    bool m_hotkeyRegistered = false;
    bool m_isController = false;
    bool m_isProbing = false;
    bool m_inputTransparent = false;
    bool m_isCaptured = false;
    bool m_isResizing = false;
    int m_activeHandle = -1;
    int m_hotkeyId = 0x52F2;

    static QList<ScreenCapturer *> s_capturers;
    static QVector<NativeWindowInfo> s_windows;
};
