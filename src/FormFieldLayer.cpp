#include "FormFieldLayer.h"

#include "PDFFormAppearance.h"

#include <QAbstractItemModel>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGTextNode>
#include <QSGTexture>
#include <QSGVertexColorMaterial>
#include <QStyleHints>
#include <QTextLayout>
#include <QTextOption>
#include <QtMath>

#include <algorithm>

namespace {

void clearNodeChildren(QSGNode *node)
{
    while (QSGNode *child = node->firstChild()) {
        node->removeChildNode(child);
        delete child;
    }
}

bool checkedValue(const QString &value)
{
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0
        || value == QStringLiteral("1");
}

struct ColoredVertex
{
    QPointF position;
    QColor color;
};

qreal alignedDeviceCoordinate(qreal coordinate, qreal devicePixelRatio)
{
    return qRound64(coordinate * devicePixelRatio)
        / devicePixelRatio;
}

QRectF alignedDeviceRect(const QRectF &rect, qreal devicePixelRatio)
{
    const qreal left = alignedDeviceCoordinate(
        rect.left(),
        devicePixelRatio);
    const qreal top = alignedDeviceCoordinate(
        rect.top(),
        devicePixelRatio);
    const qreal right = alignedDeviceCoordinate(
        rect.right(),
        devicePixelRatio);
    const qreal bottom = alignedDeviceCoordinate(
        rect.bottom(),
        devicePixelRatio);
    return QRectF(
        QPointF(left, top),
        QPointF(
            qMax(left + 1.0 / devicePixelRatio, right),
            qMax(top + 1.0 / devicePixelRatio, bottom)));
}

qreal alignedDeviceWidth(qreal width, qreal devicePixelRatio)
{
    return qMax<qreal>(
        1.0,
        qRound64(width * devicePixelRatio))
        / devicePixelRatio;
}

void appendTriangle(QVector<ColoredVertex> *vertices,
                    const QPointF &first,
                    const QPointF &second,
                    const QPointF &third,
                    const QColor &color)
{
    vertices->append({first, color});
    vertices->append({second, color});
    vertices->append({third, color});
}

void appendRoundedRect(QVector<ColoredVertex> *vertices,
                       const QRectF &rect,
                       qreal radius,
                       const QColor &color)
{
    if (rect.isEmpty()) {
        return;
    }

    radius = qBound(0.0,
                    radius,
                    qMin(rect.width(), rect.height()) / 2.0);
    if (radius <= 0.0) {
        appendTriangle(vertices,
                       rect.topLeft(),
                       rect.topRight(),
                       rect.bottomRight(),
                       color);
        appendTriangle(vertices,
                       rect.topLeft(),
                       rect.bottomRight(),
                       rect.bottomLeft(),
                       color);
        return;
    }

    constexpr int segmentsPerCorner = 4;
    QVector<QPointF> perimeter;
    perimeter.reserve(segmentsPerCorner * 4 + 4);
    const QPointF centers[] = {
        {rect.left() + radius, rect.top() + radius},
        {rect.right() - radius, rect.top() + radius},
        {rect.right() - radius, rect.bottom() - radius},
        {rect.left() + radius, rect.bottom() - radius},
    };
    const qreal startAngles[] = {180.0, 270.0, 0.0, 90.0};
    for (int corner = 0; corner < 4; ++corner) {
        for (int segment = 0; segment <= segmentsPerCorner; ++segment) {
            const qreal angle = qDegreesToRadians(
                startAngles[corner]
                + 90.0 * segment / segmentsPerCorner);
            perimeter.append({centers[corner].x() + qCos(angle) * radius,
                              centers[corner].y() + qSin(angle) * radius});
        }
    }

    for (qsizetype point = 0; point < perimeter.size(); ++point) {
        appendTriangle(vertices,
                       rect.center(),
                       perimeter.at(point),
                       perimeter.at((point + 1) % perimeter.size()),
                       color);
    }
}

void appendThickLine(QVector<ColoredVertex> *vertices,
                     const QPointF &start,
                     const QPointF &end,
                     qreal width,
                     const QColor &color)
{
    const QLineF line(start, end);
    if (line.length() <= 0.0) {
        return;
    }

    const QPointF normal(-line.dy() / line.length() * width / 2.0,
                         line.dx() / line.length() * width / 2.0);
    appendTriangle(vertices,
                   start + normal,
                   end + normal,
                   end - normal,
                   color);
    appendTriangle(vertices,
                   start + normal,
                   end - normal,
                   start - normal,
                   color);
}

QSGGeometryNode *createGeometryNode(const QVector<ColoredVertex> &vertices)
{
    if (vertices.isEmpty()) {
        return nullptr;
    }

    auto *geometry = new QSGGeometry(
        QSGGeometry::defaultAttributes_ColoredPoint2D(),
        vertices.size());
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
    QSGGeometry::ColoredPoint2D *points
        = geometry->vertexDataAsColoredPoint2D();
    for (qsizetype index = 0; index < vertices.size(); ++index) {
        const ColoredVertex &vertex = vertices.at(index);
        const int alpha = vertex.color.alpha();
        points[index].set(
            static_cast<float>(vertex.position.x()),
            static_cast<float>(vertex.position.y()),
            static_cast<uchar>(vertex.color.red() * alpha / 255),
            static_cast<uchar>(vertex.color.green() * alpha / 255),
            static_cast<uchar>(vertex.color.blue() * alpha / 255),
            static_cast<uchar>(alpha));
    }

    auto *node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlag(QSGNode::OwnsMaterial, true);
    return node;
}

void configureTextNode(QSGTextNode *node, const QRectF &viewport)
{
    node->setColor(QColor(QStringLiteral("#202124")));
    node->setTextStyle(QSGTextNode::Normal);
    node->setFiltering(QSGTexture::Linear);
    node->setViewport(viewport);
    switch (QQuickWindow::textRenderType()) {
    case QQuickWindow::NativeTextRendering:
        node->setRenderType(QSGTextNode::NativeRendering);
        break;
    case QQuickWindow::CurveTextRendering:
        node->setRenderType(QSGTextNode::CurveRendering);
        break;
    case QQuickWindow::QtTextRendering:
        node->setRenderType(QSGTextNode::QtRendering);
        break;
    }
}

void appendText(QSGTextNode *node,
                const QString &value,
                const QFont &baseFont,
                const QRectF &rect,
                int pixelSize,
                bool elide,
                Qt::Alignment horizontalAlignment = Qt::AlignLeft)
{
    if (!node || value.isEmpty() || rect.isEmpty()) {
        return;
    }

    QFont font = baseFont;
    font.setPixelSize(qMax(1, pixelSize));
    const QFontMetricsF metrics(font);
    QString displayText;
    if (elide) {
        displayText = metrics.elidedText(
            value,
            Qt::ElideRight,
            qMax(0.0, rect.width()));
    } else if (metrics.horizontalAdvance(value) <= rect.width()) {
        displayText = value;
    } else {
        qsizetype minimum = 0;
        qsizetype maximum = value.size();
        while (minimum < maximum) {
            const qsizetype candidate
                = minimum + (maximum - minimum + 1) / 2;
            if (metrics.horizontalAdvance(value.left(candidate))
                <= rect.width()) {
                minimum = candidate;
            } else {
                maximum = candidate - 1;
            }
        }
        if (minimum > 0 && value.at(minimum - 1).isHighSurrogate()) {
            --minimum;
        }
        displayText = value.left(minimum);
    }
    if (displayText.isEmpty()) {
        return;
    }

    QTextLayout layout(displayText, font);
    QTextOption textOption;
    textOption.setAlignment(horizontalAlignment);
    layout.setTextOption(textOption);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) {
        layout.endLayout();
        return;
    }
    line.setLineWidth(rect.width());
    line.setPosition({0.0, 0.0});
    layout.endLayout();
    node->addTextLayout({rect.x(),
                         rect.center().y() - line.height() / 2.0},
                        &layout);
}

} // namespace

FormFieldLayer::FormFieldLayer(QQuickItem *parent)
    : QQuickItem(parent)
    , m_renderFont(QGuiApplication::font())
    , m_textFieldRenderFont(QStringLiteral("Helvetica"))
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
}

PDFFormDesignerController *FormFieldLayer::controller() const
{
    return m_controller;
}

int FormFieldLayer::page() const
{
    return m_page;
}

bool FormFieldLayer::pooled() const
{
    return m_pooled;
}

bool FormFieldLayer::formsActive() const
{
    return m_formsActive;
}

bool FormFieldLayer::fieldsVisible() const
{
    return m_fieldsVisible;
}

bool FormFieldLayer::fieldsInteractive() const
{
    return m_fieldsInteractive;
}

bool FormFieldLayer::moveEnabled() const
{
    return m_moveEnabled;
}

bool FormFieldLayer::moving() const
{
    return m_moving;
}

bool FormFieldLayer::sourceFieldsVisible() const
{
    return m_sourceFieldsVisible;
}

qreal FormFieldLayer::pointScale() const
{
    return m_pointScale;
}

qreal FormFieldLayer::textFieldFontPointSize() const
{
    return PDFFormAppearance::kTextFieldFontPointSize;
}

qreal FormFieldLayer::textFieldBorderPointWidth() const
{
    return PDFFormAppearance::kTextFieldBorderPointWidth;
}

QColor FormFieldLayer::textFieldBorderColor() const
{
    return QColor::fromRgb(PDFFormAppearance::kTextFieldBorderRgb);
}

qreal FormFieldLayer::textFieldMinimumHorizontalPaddingPoints() const
{
    return PDFFormAppearance::kTextFieldMinimumHorizontalPaddingPoints;
}

qreal FormFieldLayer::textFieldHorizontalPaddingRatio() const
{
    return PDFFormAppearance::kTextFieldHorizontalPaddingRatio;
}

QColor FormFieldLayer::accentColor() const
{
    return m_accentColor;
}

QString FormFieldLayer::activeFieldId() const
{
    return m_activeFieldId;
}

QRectF FormFieldLayer::activeFieldRect() const
{
    return m_activeFieldRect;
}

QString FormFieldLayer::activeFieldValue() const
{
    return m_activeFieldValue;
}

QStringList FormFieldLayer::activeFieldOptions() const
{
    return m_activeFieldOptions;
}

int FormFieldLayer::activeFieldType() const
{
    return m_activeFieldType;
}

int FormFieldLayer::activeFieldFlags() const
{
    return m_activeFieldFlags;
}

bool FormFieldLayer::activeFieldSourceBacked() const
{
    return m_activeFieldSourceBacked;
}

bool FormFieldLayer::contains(const QPointF &point) const
{
    if (!QQuickItem::contains(point)
        || !m_fieldsVisible
        || !m_fieldsInteractive
        || m_pooled) {
        return false;
    }

    return std::any_of(
        m_renderFields.crbegin(),
        m_renderFields.crend(),
        [this, &point](const RenderField &field) {
            return itemRect(field.normalizedRect).contains(point);
        });
}

void FormFieldLayer::setController(PDFFormDesignerController *controller)
{
    if (m_controller == controller) {
        return;
    }
    resetPointerInteraction();
    if (m_controller) {
        disconnect(m_controller, nullptr, this, nullptr);
    }
    m_controller = controller;
    if (m_controller) {
        connect(m_controller,
                &QAbstractItemModel::rowsInserted,
                this,
                [this](const QModelIndex &, int, int) {
                    refreshFields();
                });
        connect(m_controller,
                &QAbstractItemModel::rowsRemoved,
                this,
                [this](const QModelIndex &, int, int) {
                    refreshFields();
                });
        connect(m_controller,
                &QAbstractItemModel::modelReset,
                this,
                &FormFieldLayer::refreshFields);
        connect(m_controller,
                &QAbstractItemModel::dataChanged,
                this,
                [this](const QModelIndex &topLeft,
                       const QModelIndex &bottomRight,
                       const QList<int> &roles) {
                    const bool changesFieldIdentity = roles.isEmpty()
                        || std::any_of(
                            roles.cbegin(),
                            roles.cend(),
                            [](int role) {
                                return role == PDFFormDesignerController::IdRole
                                    || role == PDFFormDesignerController::TypeRole
                                    || role == PDFFormDesignerController::PageRole
                                    || role
                                        == PDFFormDesignerController::SourcePageRole
                                    || role
                                        == PDFFormDesignerController::HasSourcePageRole;
                            });
                    if (topLeft.row() == bottomRight.row()
                        && !roles.isEmpty()) {
                        const QModelIndex modelIndex
                            = m_controller->index(topLeft.row());
                        const QString fieldId = m_controller->data(
                            modelIndex,
                            PDFFormDesignerController::IdRole).toString();
                        const int fieldType = m_controller->data(
                            modelIndex,
                            PDFFormDesignerController::TypeRole).toInt();
                        const int fieldPage = m_controller->data(
                            modelIndex,
                            PDFFormDesignerController::PageRole).toInt();
                        if (fieldPage != m_page && !changesFieldIdentity) {
                            return;
                        }
                        if (fieldId == m_activeFieldId
                            && fieldType == PDFFormDesignerController::Text
                            && fieldPage == m_page) {
                            const auto field = std::find_if(
                                m_renderFields.begin(),
                                m_renderFields.end(),
                                [&fieldId](const RenderField &candidate) {
                                    return candidate.id == fieldId;
                                });
                            if (field != m_renderFields.end()) {
                                field->normalizedRect = m_controller->data(
                                    modelIndex,
                                    PDFFormDesignerController::RectRole).toRectF();
                                field->value = m_controller->data(
                                    modelIndex,
                                    PDFFormDesignerController::ValueRole).toString();
                                field->flags = m_controller->data(
                                    modelIndex,
                                    PDFFormDesignerController::FlagsRole).toInt();
                                field->sourceBacked = m_controller->data(
                                    modelIndex,
                                    PDFFormDesignerController::SourceBackedRole)
                                                          .toBool();
                                refreshActiveField();
                                if (!changesFieldIdentity) {
                                    return;
                                }
                            }
                        }
                    }
                    refreshFields();
                });
        connect(m_controller,
                &PDFFormDesignerController::textFieldSelectionChanged,
                this,
                &FormFieldLayer::refreshFields);
        connect(m_controller,
                &QObject::destroyed,
                this,
                [this]() {
                    m_controller = nullptr;
                    resetPointerInteraction();
                    refreshFields();
                    emit controllerChanged();
                });
    }
    refreshFields();
    emit controllerChanged();
}

void FormFieldLayer::setPage(int page)
{
    if (m_page == page) {
        return;
    }
    resetPointerInteraction();
    m_page = page;
    refreshFields();
    emit pageChanged();
}

void FormFieldLayer::setPooled(bool pooled)
{
    if (m_pooled == pooled) {
        return;
    }
    resetPointerInteraction();
    m_pooled = pooled;
    refreshFields();
    emit pooledChanged();
}

void FormFieldLayer::setFormsActive(bool active)
{
    if (m_formsActive == active) {
        return;
    }
    resetPointerInteraction();
    m_formsActive = active;
    refreshFields();
    emit formsActiveChanged();
}

void FormFieldLayer::setFieldsVisible(bool visible)
{
    if (m_fieldsVisible == visible) {
        return;
    }
    resetPointerInteraction();
    m_fieldsVisible = visible;
    refreshFields();
    emit fieldsVisibleChanged();
}

void FormFieldLayer::setFieldsInteractive(bool interactive)
{
    if (m_fieldsInteractive == interactive) {
        return;
    }
    if (!interactive) {
        resetPointerInteraction();
    }
    m_fieldsInteractive = interactive;
    emit fieldsInteractiveChanged();
}

void FormFieldLayer::setMoveEnabled(bool enabled)
{
    if (m_moveEnabled == enabled) {
        return;
    }
    if (!enabled) {
        resetPointerInteraction();
    }
    m_moveEnabled = enabled;
    setAcceptHoverEvents(enabled);
    if (!enabled) {
        unsetCursor();
    }
    emit moveEnabledChanged();
}

void FormFieldLayer::setSourceFieldsVisible(bool visible)
{
    if (m_sourceFieldsVisible == visible) {
        return;
    }
    resetPointerInteraction();
    m_sourceFieldsVisible = visible;
    refreshFields();
    emit sourceFieldsVisibleChanged();
}

void FormFieldLayer::setPointScale(qreal scale)
{
    scale = qMax(0.01, scale);
    if (qFuzzyCompare(m_pointScale, scale)) {
        return;
    }
    m_pointScale = scale;
    update();
    emit pointScaleChanged();
}

void FormFieldLayer::setAccentColor(const QColor &color)
{
    if (m_accentColor == color || !color.isValid()) {
        return;
    }
    m_accentColor = color;
    update();
    emit accentColorChanged();
}

bool FormFieldLayer::beginFieldMove(const QString &fieldId)
{
    if (!m_moveEnabled || !m_controller || !m_fieldsVisible
        || !m_fieldsInteractive || m_pooled || fieldId.isEmpty()) {
        return false;
    }

    if (m_moving) {
        cancelFieldMove();
    }
    const auto field = std::find_if(
        m_renderFields.cbegin(),
        m_renderFields.cend(),
        [&fieldId](const RenderField &candidate) {
            return candidate.id == fieldId && candidate.selected;
        });
    if (field == m_renderFields.cend()
        || field->normalizedRect.isEmpty()) {
        return false;
    }

    m_movingFieldId = fieldId;
    m_moveStartRect = field->normalizedRect;
    m_movePreviewRect = m_moveStartRect;
    m_moving = true;
    emit movingChanged();
    return true;
}

bool FormFieldLayer::updateFieldMove(qreal normalizedDeltaX,
                                     qreal normalizedDeltaY)
{
    if (!m_moving
        || !qIsFinite(normalizedDeltaX)
        || !qIsFinite(normalizedDeltaY)) {
        return false;
    }

    const qreal x = qBound(
        0.0,
        m_moveStartRect.x() + normalizedDeltaX,
        qMax(0.0, 1.0 - m_moveStartRect.width()));
    const qreal y = qBound(
        0.0,
        m_moveStartRect.y() + normalizedDeltaY,
        qMax(0.0, 1.0 - m_moveStartRect.height()));
    const QRectF previewRect(
        x,
        y,
        m_moveStartRect.width(),
        m_moveStartRect.height());
    if (m_movePreviewRect == previewRect) {
        return true;
    }

    const auto field = std::find_if(
        m_renderFields.begin(),
        m_renderFields.end(),
        [this](const RenderField &candidate) {
            return candidate.id == m_movingFieldId;
        });
    if (field == m_renderFields.end()) {
        cancelFieldMove();
        return false;
    }

    m_movePreviewRect = previewRect;
    field->normalizedRect = previewRect;
    refreshActiveField();
    update();
    return true;
}

bool FormFieldLayer::finishFieldMove()
{
    if (!m_moving) {
        return false;
    }

    const QString fieldId = m_movingFieldId;
    const QPointF topLeft = m_movePreviewRect.topLeft();
    m_pressedFieldId.clear();
    setKeepMouseGrab(false);
    resetMoveState();
    const bool moved = m_controller
        && m_controller->moveField(fieldId, topLeft.x(), topLeft.y());
    refreshFields();
    return moved;
}

void FormFieldLayer::cancelFieldMove()
{
    m_pressedFieldId.clear();
    setKeepMouseGrab(false);
    if (!m_moving) {
        return;
    }
    resetMoveState();
    refreshFields();
}

QSGNode *FormFieldLayer::updatePaintNode(QSGNode *oldNode,
                                         UpdatePaintNodeData *)
{
    QSGNode *rootNode = oldNode ? oldNode : new QSGNode;
    clearNodeChildren(rootNode);
    if (!window() || !m_fieldsVisible || m_pooled || width() <= 0.0
        || height() <= 0.0) {
        return rootNode;
    }

    QVector<ColoredVertex> geometryVertices;
    geometryVertices.reserve(m_renderFields.size() * 120);
    QSGTextNode *textNode = window()->createTextNode();
    if (textNode) {
        configureTextNode(textNode, boundingRect());
    }

    const QColor defaultBorder(QStringLiteral("#7f8c99"));
    const QColor textFieldBorder
        = QColor::fromRgb(PDFFormAppearance::kTextFieldBorderRgb);
    const QColor readOnlyBackground(QStringLiteral("#f3f4f5"));
    const QColor darkControl(QStringLiteral("#5f6368"));
    const QColor signatureLine(QStringLiteral("#9aa0a6"));
    const qreal minimumTextFieldPadding
        = PDFFormAppearance::kTextFieldMinimumHorizontalPaddingPoints
        * m_pointScale;
    const qreal devicePixelRatio = qMax<qreal>(
        1.0,
        window()->devicePixelRatio());
    for (const RenderField &field : m_renderFields) {
        if (field.id == m_activeFieldId
            && field.type == PDFFormDesignerController::Text) {
            continue;
        }
        const QRectF rawFieldRect = itemRect(field.normalizedRect);
        if (rawFieldRect.isEmpty()) {
            continue;
        }

        const bool textField
            = field.type == PDFFormDesignerController::Text;
        const bool checkbox
            = field.type == PDFFormDesignerController::Checkbox;
        const bool dropdown
            = field.type == PDFFormDesignerController::Dropdown;
        // Source Widget rectangles commonly return with fractional coordinates
        // after a Save/reopen cycle. Keep both opposite edges on the same
        // physical-pixel phase so a one-point border cannot rasterize as one
        // pixel on one side and two pixels on the other.
        const QRectF fieldRect = textField || checkbox || dropdown
            ? alignedDeviceRect(rawFieldRect, devicePixelRatio)
            : rawFieldRect;
        const bool selected = m_formsActive && field.selected;
        const bool readOnly
            = (field.flags & PDFFormDesignerController::ReadOnly) != 0;
        const qreal requestedBorderWidth = selected
            ? 2.0
            : textField
                ? PDFFormAppearance::kTextFieldBorderPointWidth
                    * m_pointScale
                : 1.0;
        const qreal borderWidth = textField || checkbox || dropdown
            ? alignedDeviceWidth(
                  requestedBorderWidth,
                  devicePixelRatio)
            : requestedBorderWidth;
        const qreal fieldRadius = textField
            ? 0.0
            : checkbox
                ? 2.0
                : 5.0;
        appendRoundedRect(&geometryVertices,
                          fieldRect,
                          fieldRadius,
                          selected
                              ? m_accentColor
                              : textField
                                  ? textFieldBorder
                                  : defaultBorder);
        appendRoundedRect(&geometryVertices,
                          fieldRect.adjusted(borderWidth,
                                             borderWidth,
                                             -borderWidth,
                                             -borderWidth),
                          qMax(0.0, fieldRadius - borderWidth),
                          readOnly && !textField
                              ? readOnlyBackground
                              : Qt::white);

        const qreal horizontalPadding
            = textField
            ? qMax(
                  minimumTextFieldPadding,
                  fieldRect.height()
                      * PDFFormAppearance::kTextFieldHorizontalPaddingRatio)
            : checkbox
                ? 0.0
                : 7.0;
        const qreal verticalPadding = textField ? 0.0 : 2.0;
        QRectF contentRect = fieldRect.adjusted(
            horizontalPadding,
            verticalPadding,
            -horizontalPadding,
            -verticalPadding);
        const int defaultFontPixelSize = qRound(qBound(
            10.0,
            contentRect.height() * 0.5,
            14.0));
        switch (field.type) {
        case PDFFormDesignerController::Checkbox: {
            if (checkedValue(field.value)) {
                const qreal side = qMax(
                    4.0,
                    qMin(fieldRect.width(), fieldRect.height()));
                QFont checkboxFont = m_renderFont;
                checkboxFont.setWeight(QFont::DemiBold);
                appendText(
                    textNode,
                    QStringLiteral("\u2713"),
                    checkboxFont,
                    fieldRect.adjusted(
                        borderWidth,
                        borderWidth,
                        -borderWidth,
                        -borderWidth),
                    qRound(side * 0.78),
                    false,
                    Qt::AlignHCenter);
            }
            break;
        }
        case PDFFormDesignerController::RadioButton: {
            const qreal side = qMax(
                4.0,
                qMin(contentRect.width(), contentRect.height()));
            const QRectF circle(contentRect.x(),
                                contentRect.center().y() - side / 2.0,
                                side,
                                side);
            appendRoundedRect(&geometryVertices,
                              circle,
                              side / 2.0,
                              defaultBorder);
            appendRoundedRect(&geometryVertices,
                              circle.adjusted(1.0, 1.0, -1.0, -1.0),
                              side / 2.0 - 1.0,
                              Qt::white);
            if (checkedValue(field.value)) {
                const QRectF dot = circle.adjusted(side * 0.28,
                                                   side * 0.28,
                                                   -side * 0.28,
                                                   -side * 0.28);
                appendRoundedRect(&geometryVertices,
                                  dot,
                                  dot.width() / 2.0,
                                  m_accentColor);
            }
            break;
        }
        case PDFFormDesignerController::Dropdown: {
            const qreal arrowWidth = qMin(10.0, contentRect.height() * 0.5);
            const QPointF center(contentRect.right() - arrowWidth / 2.0,
                                 contentRect.center().y());
            appendTriangle(
                &geometryVertices,
                {center.x() - arrowWidth / 2.0,
                 center.y() - arrowWidth / 4.0},
                {center.x() + arrowWidth / 2.0,
                 center.y() - arrowWidth / 4.0},
                {center.x(), center.y() + arrowWidth / 3.0},
                darkControl);
            contentRect.adjust(0.0, 0.0, -arrowWidth - 5.0, 0.0);
            appendText(
                textNode,
                field.value,
                m_renderFont,
                contentRect,
                defaultFontPixelSize,
                true);
            break;
        }
        case PDFFormDesignerController::Signature:
            appendThickLine(&geometryVertices,
                            contentRect.bottomLeft(),
                            contentRect.bottomRight(),
                            1.0,
                            signatureLine);
            appendText(
                textNode,
                field.value,
                m_renderFont,
                contentRect,
                defaultFontPixelSize,
                true);
            break;
        case PDFFormDesignerController::Date:
            appendText(
                textNode,
                field.value,
                m_renderFont,
                contentRect,
                defaultFontPixelSize,
                true);
            break;
        case PDFFormDesignerController::Text:
            appendText(
                textNode,
                field.value,
                m_textFieldRenderFont,
                contentRect,
                qRound(
                    PDFFormAppearance::kTextFieldFontPointSize
                    * m_pointScale),
                false);
            break;
        }
    }

    if (QSGGeometryNode *geometryNode = createGeometryNode(geometryVertices)) {
        rootNode->appendChildNode(geometryNode);
    }
    if (textNode) {
        if (textNode->childCount() > 0) {
            rootNode->appendChildNode(textNode);
        } else {
            delete textNode;
        }
    }
    return rootNode;
}

void FormFieldLayer::geometryChange(const QRectF &newGeometry,
                                    const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        update();
    }
}

void FormFieldLayer::hoverMoveEvent(QHoverEvent *event)
{
    if (!m_moveEnabled || !m_fieldsInteractive || m_pooled) {
        unsetCursor();
        event->ignore();
        return;
    }

    const QPointF position = event->position();
    const bool overField = std::any_of(
        m_renderFields.crbegin(),
        m_renderFields.crend(),
        [this, &position](const RenderField &field) {
            return itemRect(field.normalizedRect).contains(position);
        });
    if (overField) {
        setCursor(Qt::SizeAllCursor);
        event->accept();
    } else {
        unsetCursor();
        event->ignore();
    }
}

void FormFieldLayer::hoverLeaveEvent(QHoverEvent *event)
{
    unsetCursor();
    event->accept();
}

void FormFieldLayer::mousePressEvent(QMouseEvent *event)
{
    if (!m_controller || !m_fieldsVisible || !m_fieldsInteractive || m_pooled
        || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const QPointF position = event->position();
    const auto hitField = std::find_if(
        m_renderFields.crbegin(),
        m_renderFields.crend(),
        [this, &position](const RenderField &field) {
            return itemRect(field.normalizedRect).contains(position);
        });
    if (hitField == m_renderFields.crend()) {
        m_controller->clearTextFieldFocus();
        m_controller->clearTextFieldSelection();
        event->ignore();
        return;
    }

    const QString fieldId = hitField->id;
    m_controller->selectTextField(fieldId);
    m_pressedFieldId = fieldId;
    m_pressPosition = position;
    setKeepMouseGrab(m_moveEnabled);
    event->accept();
}

void FormFieldLayer::mouseMoveEvent(QMouseEvent *event)
{
    if (m_pressedFieldId.isEmpty()
        || !(event->buttons() & Qt::LeftButton)) {
        event->ignore();
        return;
    }
    if (!m_moveEnabled) {
        event->ignore();
        return;
    }

    const QPointF delta = event->position() - m_pressPosition;
    if (!m_moving
        && QLineF(QPointF(), delta).length()
            >= QGuiApplication::styleHints()->startDragDistance()) {
        beginFieldMove(m_pressedFieldId);
    }
    if (m_moving) {
        updateFieldMove(
            delta.x() / qMax(1.0, width()),
            delta.y() / qMax(1.0, height()));
    }
    event->accept();
}

void FormFieldLayer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton
        || m_pressedFieldId.isEmpty()) {
        event->ignore();
        return;
    }

    const QString fieldId = m_pressedFieldId;
    const bool moved = m_moving;
    m_pressedFieldId.clear();
    setKeepMouseGrab(false);
    if (moved) {
        finishFieldMove();
    } else {
        emit fieldActivated(fieldId);
    }
    event->accept();
}

void FormFieldLayer::mouseUngrabEvent()
{
    resetPointerInteraction();
}

void FormFieldLayer::refreshFields()
{
    m_renderFields.clear();
    if (m_controller && m_fieldsVisible && !m_pooled && m_page >= 0) {
        const QString selectedFieldId = m_controller->selectedTextFieldId();
        m_renderFields.reserve(m_controller->rowCount());
        for (int row = 0; row < m_controller->rowCount(); ++row) {
            const QModelIndex modelIndex = m_controller->index(row);
            if (m_controller->data(modelIndex,
                                   PDFFormDesignerController::PageRole).toInt()
                != m_page) {
                continue;
            }

            RenderField field;
            field.id = m_controller->data(
                modelIndex,
                PDFFormDesignerController::IdRole).toString();
            field.type = m_controller->data(
                modelIndex,
                PDFFormDesignerController::TypeRole).toInt();
            field.normalizedRect = m_controller->data(
                modelIndex,
                PDFFormDesignerController::RectRole).toRectF();
            field.value = m_controller->data(
                modelIndex,
                PDFFormDesignerController::ValueRole).toString();
            field.options = m_controller->data(
                modelIndex,
                PDFFormDesignerController::OptionsRole).toStringList();
            field.flags = m_controller->data(
                modelIndex,
                PDFFormDesignerController::FlagsRole).toInt();
            field.sourceBacked = m_controller->data(
                modelIndex,
                PDFFormDesignerController::SourceBackedRole).toBool();
            if (field.sourceBacked && !m_sourceFieldsVisible) {
                continue;
            }
            field.selected = field.id == selectedFieldId;
            m_renderFields.append(std::move(field));
        }
    }
    if (m_moving) {
        const auto movingField = std::find_if(
            m_renderFields.begin(),
            m_renderFields.end(),
            [this](const RenderField &field) {
                return field.id == m_movingFieldId && field.selected;
            });
        if (movingField == m_renderFields.end()) {
            m_pressedFieldId.clear();
            setKeepMouseGrab(false);
            resetMoveState();
        } else {
            movingField->normalizedRect = m_movePreviewRect;
        }
    }
    if (!m_pressedFieldId.isEmpty()) {
        const bool pressedFieldStillSelected = std::any_of(
            m_renderFields.cbegin(),
            m_renderFields.cend(),
            [this](const RenderField &field) {
                return field.id == m_pressedFieldId && field.selected;
            });
        if (!pressedFieldStillSelected) {
            m_pressedFieldId.clear();
            setKeepMouseGrab(false);
        }
    }
    refreshActiveField();
    update();
}

void FormFieldLayer::refreshActiveField()
{
    QString fieldId;
    QRectF fieldRect;
    QString fieldValue;
    QStringList fieldOptions;
    int fieldType = -1;
    int fieldFlags = PDFFormDesignerController::NoFieldFlags;
    bool fieldSourceBacked = false;

    if (m_controller && m_fieldsVisible && !m_pooled) {
        const QString selectedFieldId = m_controller->selectedTextFieldId();
        const auto field = std::find_if(
            m_renderFields.cbegin(),
            m_renderFields.cend(),
            [&selectedFieldId](const RenderField &candidate) {
                return candidate.id == selectedFieldId;
            });
        if (field != m_renderFields.cend()) {
            fieldId = field->id;
            fieldRect = field->normalizedRect;
            fieldValue = field->value;
            fieldOptions = field->options;
            fieldType = field->type;
            fieldFlags = field->flags;
            fieldSourceBacked = field->sourceBacked;
        }
    }

    if (m_activeFieldId == fieldId && m_activeFieldRect == fieldRect
        && m_activeFieldValue == fieldValue
        && m_activeFieldOptions == fieldOptions
        && m_activeFieldType == fieldType
        && m_activeFieldFlags == fieldFlags
        && m_activeFieldSourceBacked == fieldSourceBacked) {
        return;
    }
    m_activeFieldId = fieldId;
    m_activeFieldRect = fieldRect;
    m_activeFieldValue = fieldValue;
    m_activeFieldOptions = fieldOptions;
    m_activeFieldType = fieldType;
    m_activeFieldFlags = fieldFlags;
    m_activeFieldSourceBacked = fieldSourceBacked;
    emit activeFieldChanged();
}

void FormFieldLayer::resetMoveState()
{
    if (!m_moving) {
        return;
    }
    m_moving = false;
    m_movingFieldId.clear();
    m_moveStartRect = {};
    m_movePreviewRect = {};
    emit movingChanged();
}

void FormFieldLayer::resetPointerInteraction()
{
    cancelFieldMove();
}

QRectF FormFieldLayer::itemRect(const QRectF &normalizedRect) const
{
    return QRectF(normalizedRect.x() * width(),
                  normalizedRect.y() * height(),
                  normalizedRect.width() * width(),
                  normalizedRect.height() * height());
}
