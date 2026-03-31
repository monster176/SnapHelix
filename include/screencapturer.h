#pragma once

#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QPixmap>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

class QColor;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QPainterPath;
class QScreen;
class QShowEvent;
class QWidget;

class ScreenCapturer final : public QWidget, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    struct NativeWindowInfo
    {
        QRect rect;
    };

    enum ToolType {
        Tool_Select,
        Tool_Draw,
        Tool_Text,
        Tool_ColorPicker,
        Tool_Arrow,
        Tool_Rectangle,
        Tool_Ellipse,
        Tool_Undo,
        Tool_Redo,
        Tool_Copy,
        Tool_Pin,
        Tool_Save,
        Tool_Close,
        Tool_COUNT
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
    struct Annotation {
        enum Kind {
            Path,
            Arrow,
            Rectangle,
            Ellipse,
            Text
        };

        Kind kind;
        QColor color;
        QVector<QPointF> points;
        QRect rect;
        QString text;
        QPoint textPos;
    };

    struct ToolButton {
        QRect rect;
        ToolType type;
    };

    struct ColorSwatchButton {
        QRect rect;
        QColor color;
    };

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
    void drawToolbar(QPainter &painter, const QRect &selection);
    void drawColorPalette(QPainter &painter, const QRect &selection);
    void drawAnnotations(QPainter &painter, const QRect &selection) const;
    void drawAnnotation(QPainter &painter, const QRect &selection, const Annotation &annotation) const;
    ToolType getToolAt(const QPoint &pos) const;
    QColor getColorAt(const QPoint &pos) const;
    void handleToolClick(ToolType tool);
    bool isPointInSelection(const QPoint &pos) const;
    QRect selectionBounds() const;
    QPoint mapToSelection(const QPoint &pos) const;
    QRect mapRectToSelection(const QRect &rect) const;
    bool beginAnnotationInteraction(const QPoint &localPos);
    void updateAnnotationInteraction(const QPoint &localPos);
    void finishAnnotationInteraction(const QPoint &localPos);
    void clearRedoStack();
    void copySelectionToClipboard();
    bool saveSelectionToFile();
    void pinSelection();
    void beginTextEditing(const QPoint &localPos);
    void commitTextEditing();
    void cancelTextEditing();
    void showCopyToast(const QString &text);
    QImage renderSelectionImage(bool includeDevicePixelRatio = true) const;

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
    QPoint m_annotationStart;

    QRect m_hoveredRect;
    QRect m_adjustStartRect;
    QRect m_clickSelectionCandidate;
    QRect m_annotationPreviewRect;
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
    bool m_isAnnotating = false;
    bool m_ignoreNextMouseRelease = false;
    int m_activeHandle = -1;
    int m_hotkeyId = 0x52F2;
    bool m_showToolbar = false;
    QVector<ToolButton> m_toolButtons;
    QVector<ColorSwatchButton> m_colorButtons;
    ToolType m_selectedTool = Tool_Select;
    ToolType m_hoveredToolbarTool = Tool_COUNT;
    QColor m_annotationColor = QColor(255, 59, 48);
    QVector<Annotation> m_annotations;
    QVector<Annotation> m_redoAnnotations;
    Annotation m_annotationPreview{Annotation::Path, QColor(255, 59, 48), {}, {}, {}, {}};
    QPointer<QLineEdit> m_textEditor;
    QPointer<QLabel> m_copyToastLabel;
    QTimer *m_copyToastTimer = nullptr;
    QPoint m_textEditAnchor;
    QList<QPointer<QWidget>> m_pinnedWindows;

    static QList<ScreenCapturer *> s_capturers;
    static QVector<NativeWindowInfo> s_windows;
};
