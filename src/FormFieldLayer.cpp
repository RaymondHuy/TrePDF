#include "FormFieldLayer.h"

#include <QAbstractItemModel>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLineF>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGTextNode>
#include <QSGTexture>
#include <QSGVertexColorMaterial>
#include <QTextLayout>
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
        || value == QStringLiteral("1");
}

struct ColoredVertex
{
    QPointF position;
    QColor color;
};

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
                const QRectF &rect)
{
    if (!node || value.isEmpty() || rect.isEmpty()) {
        return;
    }

    QFont font = baseFont;
    font.setPixelSize(qRound(qBound(10.0, rect.height() * 0.5, 14.0)));
    const QFontMetricsF metrics(font);
    const QString elidedText = metrics.elidedText(
        value,
        Qt::ElideRight,
        qMax(0.0, rect.width()));
    if (elidedText.isEmpty()) {
        return;
    }

    QTextLayout layout(elidedText, font);
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

bool FormFieldLayer::sourceFieldsVisible() const
{
    return m_sourceFieldsVisible;
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

void FormFieldLayer::setController(PDFFormDesignerController *controller)
{
    if (m_controller == controller) {
        return;
    }
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
    m_page = page;
    refreshFields();
    emit pageChanged();
}

void FormFieldLayer::setPooled(bool pooled)
{
    if (m_pooled == pooled) {
        return;
    }
    m_pooled = pooled;
    refreshFields();
    emit pooledChanged();
}

void FormFieldLayer::setFormsActive(bool active)
{
    if (m_formsActive == active) {
        return;
    }
    m_formsActive = active;
    refreshFields();
    emit formsActiveChanged();
}

void FormFieldLayer::setFieldsVisible(bool visible)
{
    if (m_fieldsVisible == visible) {
        return;
    }
    m_fieldsVisible = visible;
    refreshFields();
    emit fieldsVisibleChanged();
}

void FormFieldLayer::setFieldsInteractive(bool interactive)
{
    if (m_fieldsInteractive == interactive) {
        return;
    }
    m_fieldsInteractive = interactive;
    emit fieldsInteractiveChanged();
}

void FormFieldLayer::setSourceFieldsVisible(bool visible)
{
    if (m_sourceFieldsVisible == visible) {
        return;
    }
    m_sourceFieldsVisible = visible;
    refreshFields();
    emit sourceFieldsVisibleChanged();
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
    const QColor readOnlyBackground(QStringLiteral("#f3f4f5"));
    const QColor darkControl(QStringLiteral("#5f6368"));
    const QColor signatureLine(QStringLiteral("#9aa0a6"));
    for (const RenderField &field : m_renderFields) {
        if (field.id == m_activeFieldId
            && field.type == PDFFormDesignerController::Text) {
            continue;
        }
        const QRectF fieldRect = itemRect(field.normalizedRect);
        if (fieldRect.isEmpty()) {
            continue;
        }

        const bool selected = m_formsActive && field.selected;
        const bool readOnly
            = (field.flags & PDFFormDesignerController::ReadOnly) != 0;
        const qreal borderWidth = selected ? 2.0 : 1.0;
        const qreal fieldRadius
            = field.type == PDFFormDesignerController::Text ? 0.0 : 5.0;
        appendRoundedRect(&geometryVertices,
                          fieldRect,
                          fieldRadius,
                          selected ? m_accentColor : defaultBorder);
        appendRoundedRect(&geometryVertices,
                          fieldRect.adjusted(borderWidth,
                                             borderWidth,
                                             -borderWidth,
                                             -borderWidth),
                          qMax(0.0, fieldRadius - borderWidth),
                          readOnly ? readOnlyBackground : Qt::white);

        QRectF contentRect = fieldRect.adjusted(7.0, 2.0, -7.0, -2.0);
        switch (field.type) {
        case PDFFormDesignerController::Checkbox: {
            const qreal side = qMax(
                4.0,
                qMin(contentRect.width(), contentRect.height()));
            const QRectF box(contentRect.x(),
                             contentRect.center().y() - side / 2.0,
                             side,
                             side);
            appendRoundedRect(&geometryVertices,
                              box,
                              2.0,
                              defaultBorder);
            appendRoundedRect(&geometryVertices,
                              box.adjusted(1.0, 1.0, -1.0, -1.0),
                              1.0,
                              Qt::white);
            if (checkedValue(field.value)) {
                const QPointF start(box.left() + side * 0.2,
                                    box.center().y());
                const QPointF middle(box.left() + side * 0.43,
                                     box.bottom() - side * 0.2);
                const QPointF end(box.right() - side * 0.16,
                                  box.top() + side * 0.2);
                appendThickLine(&geometryVertices,
                                start,
                                middle,
                                2.0,
                                m_accentColor);
                appendThickLine(&geometryVertices,
                                middle,
                                end,
                                2.0,
                                m_accentColor);
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
            appendText(textNode, field.value, m_renderFont, contentRect);
            break;
        }
        case PDFFormDesignerController::Signature:
            appendThickLine(&geometryVertices,
                            contentRect.bottomLeft(),
                            contentRect.bottomRight(),
                            1.0,
                            signatureLine);
            appendText(textNode, field.value, m_renderFont, contentRect);
            break;
        case PDFFormDesignerController::Text:
        case PDFFormDesignerController::Date:
            appendText(textNode, field.value, m_renderFont, contentRect);
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

    m_controller->selectTextField(hitField->id);
    emit fieldActivated(hitField->id);
    event->accept();
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
    refreshActiveField();
    update();
}

void FormFieldLayer::refreshActiveField()
{
    QString fieldId;
    QRectF fieldRect;
    QString fieldValue;
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
            fieldType = field->type;
            fieldFlags = field->flags;
            fieldSourceBacked = field->sourceBacked;
        }
    }

    if (m_activeFieldId == fieldId && m_activeFieldRect == fieldRect
        && m_activeFieldValue == fieldValue
        && m_activeFieldType == fieldType
        && m_activeFieldFlags == fieldFlags
        && m_activeFieldSourceBacked == fieldSourceBacked) {
        return;
    }
    m_activeFieldId = fieldId;
    m_activeFieldRect = fieldRect;
    m_activeFieldValue = fieldValue;
    m_activeFieldType = fieldType;
    m_activeFieldFlags = fieldFlags;
    m_activeFieldSourceBacked = fieldSourceBacked;
    emit activeFieldChanged();
}

QRectF FormFieldLayer::itemRect(const QRectF &normalizedRect) const
{
    return QRectF(normalizedRect.x() * width(),
                  normalizedRect.y() * height(),
                  normalizedRect.width() * width(),
                  normalizedRect.height() * height());
}
