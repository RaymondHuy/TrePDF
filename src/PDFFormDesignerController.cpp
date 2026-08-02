#include "PDFFormDesignerController.h"

#include "PDFDocument.h"
#include "PDFFormAppearance.h"
#include "PDFFormController.h"

#include <QCoreApplication>
#include <QSet>
#include <QUuid>
#include <QVariantMap>
#include <QtGlobal>

#include <algorithm>
#include <utility>

#include <fpdf_annot.h>

namespace {

constexpr int kMaximumDropdownOptions = 4096;
constexpr qsizetype kMaximumDropdownOptionCharacters = 1024 * 1024;

QStringList defaultDropdownOptions()
{
    QStringList options;
    //% "Option 1"
    options.append(qtTrId("DropdownOption1"));
    //% "Option 2"
    options.append(qtTrId("DropdownOption2"));
    //% "Option 3"
    options.append(qtTrId("DropdownOption3"));
    return options;
}

bool validDropdownOptions(const QStringList &options)
{
    if (options.isEmpty() || options.size() > kMaximumDropdownOptions) {
        return false;
    }
    QSet<QString> uniqueOptions;
    qsizetype totalCharacters = 0;
    for (const QString &option : options) {
        totalCharacters += option.size();
        if (option.isEmpty()
            || totalCharacters > kMaximumDropdownOptionCharacters
            || uniqueOptions.contains(option)) {
            return false;
        }
        uniqueOptions.insert(option);
    }
    return true;
}

QRectF boundedFieldRect(qreal x,
                        qreal y,
                        qreal width,
                        qreal height)
{
    const QRectF normalizedRect = QRectF(x, y, width, height).normalized();
    const qreal left = qBound(0.0, normalizedRect.left(), 1.0);
    const qreal top = qBound(0.0, normalizedRect.top(), 1.0);
    const qreal right = qBound(0.0, normalizedRect.right(), 1.0);
    const qreal bottom = qBound(0.0, normalizedRect.bottom(), 1.0);
    if (right <= left || bottom <= top) {
        return {};
    }
    return QRectF(left, top, right - left, bottom - top);
}

QString fieldIdPrefix(PDFFormDesignerController::FieldType type)
{
    switch (type) {
    case PDFFormDesignerController::Text:
        return QStringLiteral("text-field");
    case PDFFormDesignerController::Checkbox:
        return QStringLiteral("checkbox-field");
    case PDFFormDesignerController::RadioButton:
        return QStringLiteral("radio-field");
    case PDFFormDesignerController::Dropdown:
        return QStringLiteral("dropdown-field");
    case PDFFormDesignerController::Date:
        return QStringLiteral("date-field");
    case PDFFormDesignerController::Signature:
        return QStringLiteral("signature-field");
    }
    return QStringLiteral("form-field");
}

QString fieldNamePrefix(PDFFormDesignerController::FieldType type)
{
    switch (type) {
    case PDFFormDesignerController::Text:
        return QStringLiteral("TextField");
    case PDFFormDesignerController::Checkbox:
        return QStringLiteral("Checkbox");
    case PDFFormDesignerController::RadioButton:
        return QStringLiteral("RadioButton");
    case PDFFormDesignerController::Dropdown:
        return QStringLiteral("Dropdown");
    case PDFFormDesignerController::Date:
        return QStringLiteral("DateField");
    case PDFFormDesignerController::Signature:
        return QStringLiteral("SignatureField");
    }
    return QStringLiteral("FormField");
}

} // namespace

PDFFormDesignerController::PDFFormDesignerController(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PDFFormDesignerController::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_fields.size();
}

QVariant PDFFormDesignerController::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_fields.size()) {
        return {};
    }

    const FormField &field = m_fields.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case ValueRole:
        return field.value;
    case IdRole:
        return field.id;
    case TypeRole:
        return field.type;
    case PageRole:
        return displayPageForField(field);
    case SourcePageRole:
        return field.sourcePage;
    case HasSourcePageRole:
        return field.hasSourcePage;
    case RectRole:
        return field.normalizedRect;
    case XRole:
        return field.normalizedRect.x();
    case YRole:
        return field.normalizedRect.y();
    case WidthRole:
        return field.normalizedRect.width();
    case HeightRole:
        return field.normalizedRect.height();
    case NameRole:
        return field.name;
    case OptionsRole:
        return field.options;
    case FlagsRole:
        return field.flags;
    case DirtyRole:
        return field.changedProperties != NoPropertyChanges;
    case SourceBackedRole:
        return field.sourceBacked;
    default:
        return {};
    }
}

QHash<int, QByteArray> PDFFormDesignerController::roleNames() const
{
    return {
        {IdRole, "id"},
        {TypeRole, "type"},
        {PageRole, "page"},
        {SourcePageRole, "sourcePage"},
        {HasSourcePageRole, "hasSourcePage"},
        {RectRole, "rect"},
        {XRole, "x"},
        {YRole, "y"},
        {WidthRole, "width"},
        {HeightRole, "height"},
        {ValueRole, "value"},
        {NameRole, "name"},
        {OptionsRole, "options"},
        {FlagsRole, "flags"},
        {DirtyRole, "dirty"},
        {SourceBackedRole, "sourceBacked"},
    };
}

bool PDFFormDesignerController::textFieldFocused() const
{
    return !m_focusedTextFieldId.isEmpty();
}

QString PDFFormDesignerController::focusedTextFieldId() const
{
    return m_focusedTextFieldId;
}

QString PDFFormDesignerController::selectedTextFieldId() const
{
    return m_selectedTextFieldId;
}

void PDFFormDesignerController::setFormController(
    PDFFormController *formController)
{
    if (m_formController == formController) {
        return;
    }
    if (m_formController) {
        disconnect(m_formController, nullptr, this, nullptr);
    }
    m_formController = formController;
    if (m_formController) {
        connect(m_formController, &QObject::destroyed, this, [this]() {
            m_formController = nullptr;
            reloadSourceFields();
        });
    }
    reloadSourceFields();
}

void PDFFormDesignerController::setDocument(PDFDocument *document)
{
    if (m_document == document) {
        return;
    }

    clear();
    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }
    m_document = document;
    if (!m_document) {
        return;
    }

    connect(m_document,
            &PDFDocument::pageStructureChanged,
            this,
            [this](bool) {
                handlePageStructureChanged();
            });
    connect(m_document,
            &PDFDocument::sourceChanged,
            this,
            &PDFFormDesignerController::reloadSourceFields);
    connect(m_document,
            &PDFDocument::loadedChanged,
            this,
            &PDFFormDesignerController::reloadSourceFields);
    connect(m_document,
            &QObject::destroyed,
            this,
            [this]() {
                m_document = nullptr;
                reloadSourceFields();
            });
    reloadSourceFields();
}

void PDFFormDesignerController::prepareForDesignMode()
{
    if (!m_formController
        || !m_document
        || m_formController->document() != m_document) {
        return;
    }
    m_formController->killFocus();
    refreshSourceFieldValues();
}

void PDFFormDesignerController::refreshSourceFieldValues()
{
    if (m_sourceValueRefreshPending) {
        m_sourceValueRefreshQueued = true;
        return;
    }
    if (std::none_of(
            m_fields.cbegin(),
            m_fields.cend(),
            [](const FormField &field) {
                return field.sourceBacked;
            })
        || !m_document
        || !m_document->isLoaded()
        || !m_formController
        || m_formController->document() != m_document) {
        return;
    }

    m_sourceValueRefreshPending = true;
    const quint64 requestSerial = ++m_sourceValueRefreshSerial;
    QPointer<PDFFormDesignerController> controller(this);
    QPointer<PDFDocument> document(m_document);
    m_formController->requestTrePDFTextFields(
        [controller, document, requestSerial](
            bool succeeded,
            QVector<PDFFormWorker::TrePDFTextField> sourceFields) {
            if (!controller
                || controller->m_sourceValueRefreshSerial
                    != requestSerial) {
                return;
            }
            controller->m_sourceValueRefreshPending = false;
            const bool refreshQueued = std::exchange(
                controller->m_sourceValueRefreshQueued,
                false);
            if (!document
                || controller->m_document != document
                || !succeeded) {
                if (refreshQueued) {
                    controller->refreshSourceFieldValues();
                }
                return;
            }

            for (int row = 0; row < controller->m_fields.size(); ++row) {
                FormField &field = controller->m_fields[row];
                if (!field.sourceBacked) {
                    continue;
                }
                const auto source = std::find_if(
                    sourceFields.cbegin(),
                    sourceFields.cend(),
                    [&field](
                        const PDFFormWorker::TrePDFTextField &candidate) {
                        return candidate.id == field.id;
                    });
                if (source == sourceFields.cend()) {
                    continue;
                }
                field.sourceAnnotationIndex = source->annotationIndex;
                field.sourceIdentityToken = source->identityToken;
                QList<int> changedRoles;
                if (!(field.changedProperties & ValuePropertyChanged)
                    && source->value != field.value) {
                    field.value = source->value;
                    changedRoles.append(ValueRole);
                }
                if (field.type == Dropdown
                    && !(field.changedProperties & OptionsPropertyChanged)
                    && source->options != field.options) {
                    field.options = source->options;
                    changedRoles.append(OptionsRole);
                }
                if (!changedRoles.isEmpty()) {
                    emit controller->dataChanged(
                        controller->index(row),
                        controller->index(row),
                        changedRoles);
                }
            }
            if (refreshQueued) {
                controller->refreshSourceFieldValues();
            }
        });
}

QString PDFFormDesignerController::addField(int type,
                                            int page,
                                            qreal x,
                                            qreal y,
                                            qreal width,
                                            qreal height)
{
    if (type < Text || type > Signature || page < 0) {
        return {};
    }

    const QRectF normalizedRect = boundedFieldRect(x, y, width, height);
    if (normalizedRect.isEmpty()) {
        return {};
    }

    FormField field;
    field.type = static_cast<FieldType>(type);
    const quint64 sequence = ++m_nextFieldId;
    const QString uniqueSuffix = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    field.id = QStringLiteral("%1-%2")
                   .arg(fieldIdPrefix(field.type))
                   .arg(uniqueSuffix);
    field.name = QStringLiteral("%1%2_%3")
                     .arg(fieldNamePrefix(field.type))
                     .arg(sequence)
                     .arg(uniqueSuffix.left(8));
    field.page = page;
    if (m_document) {
        field.sourcePage = m_document->sourcePageIndex(page);
        field.hasSourcePage = field.sourcePage >= 0;
    }
    field.normalizedRect = normalizedRect;
    if (field.type == Checkbox) {
        field.value = QStringLiteral("Off");
        field.defaultValue = field.value;
        field.options = {
            QStringLiteral("Yes"),
            QStringLiteral("Off"),
        };
    } else if (field.type == Dropdown) {
        field.options = defaultDropdownOptions();
        field.value = field.options.constFirst();
        field.defaultValue = field.value;
    }

    const int row = m_fields.size();
    beginInsertRows({}, row, row);
    m_fields.append(field);
    endInsertRows();
    emit textFieldsChanged(page);
    emit draftStateChanged();
    return field.id;
}

QString PDFFormDesignerController::addTextField(int page,
                                                qreal x,
                                                qreal y,
                                                qreal width,
                                                qreal height)
{
    return addField(Text, page, x, y, width, height);
}

QVariantMap PDFFormDesignerController::fieldData(const QString &fieldId) const
{
    const int row = rowForFieldId(fieldId);
    return row >= 0 ? fieldToMap(m_fields.at(row)) : QVariantMap{};
}

bool PDFFormDesignerController::activateSourceCheckbox(
    const QString &fieldId,
    qreal viewWidth,
    qreal viewHeight)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0
        || !m_document
        || !m_document->isLoaded()
        || !m_formController
        || m_formController->document() != m_document
        || !m_formController->interactive()
        || viewWidth <= 0.0
        || viewHeight <= 0.0) {
        return false;
    }

    const FormField &field = m_fields.at(row);
    if (field.type != Checkbox
        || !field.sourceBacked
        || field.sourceNormalizedRect.isEmpty()
        || (field.flags & ReadOnly)) {
        return false;
    }

    const int page = displayPageForField(field);
    if (page < 0) {
        return false;
    }

    const QPointF center = field.sourceNormalizedRect.center();
    const QPointF position(center.x() * viewWidth,
                           center.y() * viewHeight);
    const QSize deviceSize(qMax(1, qRound(viewWidth)),
                           qMax(1, qRound(viewHeight)));
    m_formController->pointerEvent(
        page,
        PDFFormWorker::PointerEventType::Press,
        0,
        position,
        deviceSize);
    m_formController->pointerEvent(
        page,
        PDFFormWorker::PointerEventType::Release,
        0,
        position,
        deviceSize);
    // The designer overlay owns selection while source Widgets are hidden.
    // Leaving PDFium focused would make FPDF_FFLDraw paint its native focus
    // adornment at sourceNormalizedRect, which is the stale pre-move position
    // until the form draft is saved.
    m_formController->killFocus();
    refreshSourceFieldValues();
    return true;
}

bool PDFFormDesignerController::activateSourceDropdown(
    const QString &fieldId,
    int optionIndex)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0
        || !m_document
        || !m_document->isLoaded()
        || !m_formController
        || m_formController->document() != m_document
        || !m_formController->interactive()) {
        return false;
    }

    const FormField &field = m_fields.at(row);
    if (field.type != Dropdown
        || !field.sourceBacked
        || !field.hasSourcePage
        || field.sourcePage < 0
        || field.sourceAnnotationIndex < 0
        || field.sourceIdentityToken.isEmpty()
        || optionIndex < 0
        || optionIndex >= field.options.size()
        || (field.flags & ReadOnly)) {
        return false;
    }

    const int page = displayPageForField(field);
    if (page < 0) {
        return false;
    }
    m_formController->selectChoiceOption(
        page,
        field.sourceAnnotationIndex,
        optionIndex,
        true,
        field.sourcePage,
        false,
        field.sourceIdentityToken);
    // Keep native focus adornments out of the designer-owned source Widget.
    // Both requests run on the serialized form worker, so the selection is
    // committed before focus is cleared and the refreshed value is scanned.
    m_formController->killFocus();
    refreshSourceFieldValues();
    return true;
}

QVariantList PDFFormDesignerController::fieldsForPage(int page) const
{
    QVariantList fields;
    for (const FormField &field : m_fields) {
        if (displayPageForField(field) == page) {
            fields.append(fieldToMap(field));
        }
    }
    return fields;
}

QVariantList PDFFormDesignerController::textFieldsForPage(int page) const
{
    QVariantList fields;
    for (const FormField &field : m_fields) {
        if (field.type != Text || displayPageForField(field) != page) {
            continue;
        }

        QVariantMap value = fieldToMap(field);
        value.insert(QStringLiteral("text"), field.value);
        fields.append(value);
    }
    return fields;
}

QStringList PDFFormDesignerController::sourceFieldIdsForPage(int page) const
{
    QStringList fieldIds;
    for (const FormField &field : m_fields) {
        if (field.sourceBacked && displayPageForField(field) == page) {
            fieldIds.append(field.id);
        }
    }
    for (const FormField &field : m_removedSourceFields) {
        if (displayPageForField(field) == page) {
            fieldIds.append(field.id);
        }
    }
    return fieldIds;
}

bool PDFFormDesignerController::managesSourceChoiceField(
    int sourcePage,
    int annotationIndex,
    const QString &identityToken) const
{
    if (sourcePage < 0 || annotationIndex < 0 || identityToken.isEmpty()) {
        return false;
    }

    const auto matches =
        [sourcePage, annotationIndex, &identityToken](const FormField &field) {
            return field.type == Dropdown
                && field.sourceBacked
                && field.sourcePage == sourcePage
                && field.sourceAnnotationIndex == annotationIndex
                && field.sourceIdentityToken == identityToken;
        };
    return std::any_of(m_fields.cbegin(), m_fields.cend(), matches)
        || std::any_of(m_removedSourceFields.cbegin(),
                       m_removedSourceFields.cend(),
                       matches);
}

bool PDFFormDesignerController::managesSourceWidget(
    int page,
    int annotationIndex,
    const QString &widgetId) const
{
    if (page < 0 || annotationIndex < 0 || widgetId.isEmpty()) {
        return false;
    }

    const auto matches =
        [this, page, annotationIndex, &widgetId](const FormField &field) {
            return field.sourceBacked
                && field.id == widgetId
                && displayPageForField(field) == page
                && field.sourceAnnotationIndex == annotationIndex;
        };
    return std::any_of(m_fields.cbegin(), m_fields.cend(), matches)
        || std::any_of(m_removedSourceFields.cbegin(),
                       m_removedSourceFields.cend(),
                       matches);
}

bool PDFFormDesignerController::sourceFieldContainsPoint(
    int page,
    const QPointF &normalizedPoint) const
{
    if (page < 0
        || normalizedPoint.x() < 0.0
        || normalizedPoint.x() > 1.0
        || normalizedPoint.y() < 0.0
        || normalizedPoint.y() > 1.0) {
        return false;
    }

    const auto containsPoint =
        [this, page, &normalizedPoint](const FormField &field) {
            return field.sourceBacked
                && displayPageForField(field) == page
                && field.sourceNormalizedRect.contains(normalizedPoint);
        };
    return std::any_of(m_fields.cbegin(), m_fields.cend(), containsPoint)
        || std::any_of(m_removedSourceFields.cbegin(),
                       m_removedSourceFields.cend(),
                       containsPoint);
}

QStringList PDFFormDesignerController::sourceTextFieldIdsForPage(
    int page) const
{
    QStringList fieldIds;
    for (const FormField &field : m_fields) {
        if (field.type == Text
            && field.sourceBacked
            && displayPageForField(field) == page) {
            fieldIds.append(field.id);
        }
    }
    return fieldIds;
}

bool PDFFormDesignerController::sourceTextFieldContainsPoint(
    int page,
    const QPointF &normalizedPoint) const
{
    if (page < 0
        || normalizedPoint.x() < 0.0
        || normalizedPoint.x() > 1.0
        || normalizedPoint.y() < 0.0
        || normalizedPoint.y() > 1.0) {
        return false;
    }

    return std::any_of(
        m_fields.cbegin(),
        m_fields.cend(),
        [this, page, &normalizedPoint](const FormField &field) {
            return field.type == Text
                && field.sourceBacked
                && displayPageForField(field) == page
                && field.sourceNormalizedRect.contains(normalizedPoint);
        });
}

bool PDFFormDesignerController::setFieldValue(const QString &fieldId,
                                              const QString &value)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0) {
        return false;
    }

    FormField &field = m_fields[row];
    if (field.type == Dropdown && !field.options.contains(value)) {
        return false;
    }
    if (field.value == value) {
        return true;
    }
    field.value = value;
    field.changedProperties |= ValuePropertyChanged;
    emit dataChanged(index(row), index(row), {ValueRole, DirtyRole});
    emit draftStateChanged();
    return true;
}

bool PDFFormDesignerController::setFieldName(const QString &fieldId,
                                             const QString &name)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0) {
        return false;
    }

    FormField &field = m_fields[row];
    if (field.name == name) {
        return true;
    }
    field.name = name;
    field.changedProperties |= NamePropertyChanged;
    emit dataChanged(index(row), index(row), {NameRole, DirtyRole});
    emit draftStateChanged();
    return true;
}

bool PDFFormDesignerController::setFieldOptions(const QString &fieldId,
                                                const QStringList &options)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0) {
        return false;
    }

    FormField &field = m_fields[row];
    if (field.type == Dropdown && !validDropdownOptions(options)) {
        return false;
    }
    if (field.options == options) {
        return true;
    }
    field.options = options;
    field.changedProperties |= OptionsPropertyChanged;
    QList<int> changedRoles{OptionsRole, DirtyRole};
    if (field.type == Dropdown) {
        if (!field.options.contains(field.value)) {
            field.value = field.options.constFirst();
            field.changedProperties |= ValuePropertyChanged;
            changedRoles.append(ValueRole);
        }
        if (!field.options.contains(field.defaultValue)) {
            field.defaultValue = field.options.constFirst();
        }
    }
    emit dataChanged(index(row), index(row), changedRoles);
    emit draftStateChanged();
    return true;
}

bool PDFFormDesignerController::renameFieldOption(const QString &fieldId,
                                                  int optionIndex,
                                                  const QString &option)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0) {
        return false;
    }

    FormField &field = m_fields[row];
    if (field.type != Dropdown || optionIndex < 0
        || optionIndex >= field.options.size()) {
        return false;
    }

    const QString previousOption = field.options.at(optionIndex);
    if (previousOption == option) {
        return true;
    }

    QStringList options = field.options;
    options[optionIndex] = option;
    if (!validDropdownOptions(options)) {
        return false;
    }

    field.options = std::move(options);
    field.changedProperties |= OptionsPropertyChanged;
    QList<int> changedRoles{OptionsRole, DirtyRole};
    if (field.value == previousOption) {
        field.value = option;
        field.changedProperties |= ValuePropertyChanged;
        changedRoles.append(ValueRole);
    }
    if (field.defaultValue == previousOption) {
        field.defaultValue = option;
    }

    emit dataChanged(index(row), index(row), changedRoles);
    emit draftStateChanged();
    return true;
}

bool PDFFormDesignerController::setFieldFlags(const QString &fieldId, int flags)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0) {
        return false;
    }

    const int supportedFlags = Required | ReadOnly;
    flags &= supportedFlags;
    FormField &field = m_fields[row];
    if (field.flags == flags) {
        return true;
    }
    field.flags = flags;
    field.changedProperties |= FlagsPropertyChanged;
    emit dataChanged(index(row), index(row), {FlagsRole, DirtyRole});
    emit draftStateChanged();
    return true;
}

void PDFFormDesignerController::setTextFieldText(const QString &fieldId,
                                                 const QString &text)
{
    setFieldValue(fieldId, text);
}

bool PDFFormDesignerController::setFieldRect(const QString &fieldId,
                                             qreal x,
                                             qreal y,
                                             qreal width,
                                             qreal height)
{
    const QRectF normalizedRect = boundedFieldRect(x, y, width, height);
    if (normalizedRect.isEmpty()) {
        return false;
    }

    const int row = rowForFieldId(fieldId);
    if (row < 0) {
        return false;
    }

    FormField &field = m_fields[row];
    if (field.normalizedRect == normalizedRect) {
        return true;
    }
    field.normalizedRect = normalizedRect;
    field.changedProperties |= RectPropertyChanged;
    emit dataChanged(index(row),
                     index(row),
                     {RectRole, XRole, YRole, WidthRole, HeightRole, DirtyRole});
    const int page = displayPageForField(field);
    emit textFieldsChanged(page >= 0 ? page : -1);
    emit draftStateChanged();
    return true;
}

bool PDFFormDesignerController::moveField(const QString &fieldId,
                                          qreal x,
                                          qreal y)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0 || !qIsFinite(x) || !qIsFinite(y)) {
        return false;
    }

    const QRectF fieldRect = m_fields.at(row).normalizedRect;
    const qreal boundedX = qBound(
        0.0,
        x,
        qMax(0.0, 1.0 - fieldRect.width()));
    const qreal boundedY = qBound(
        0.0,
        y,
        qMax(0.0, 1.0 - fieldRect.height()));
    return setFieldRect(fieldId,
                        boundedX,
                        boundedY,
                        fieldRect.width(),
                        fieldRect.height());
}

bool PDFFormDesignerController::setTextFieldRect(const QString &fieldId,
                                                 qreal x,
                                                 qreal y,
                                                 qreal width,
                                                 qreal height)
{
    return setFieldRect(fieldId, x, y, width, height);
}

bool PDFFormDesignerController::removeField(const QString &fieldId)
{
    const int row = rowForFieldId(fieldId);
    if (row < 0) {
        return false;
    }

    const FormField &field = m_fields.at(row);
    if (field.sourceBacked && field.type != Dropdown) {
        return false;
    }

    const int page = displayPageForField(field);
    if (m_focusedTextFieldId == fieldId) {
        clearTextFieldFocus();
    }
    if (m_selectedTextFieldId == fieldId) {
        clearTextFieldSelection();
    }
    if (field.sourceBacked) {
        m_removedSourceFields.append(field);
    }
    beginRemoveRows({}, row, row);
    m_fields.removeAt(row);
    endRemoveRows();
    emit textFieldsChanged(page >= 0 ? page : -1);
    emit draftStateChanged();
    return true;
}

bool PDFFormDesignerController::removeTextField(const QString &fieldId)
{
    return removeField(fieldId);
}

void PDFFormDesignerController::selectTextField(const QString &fieldId)
{
    if (fieldId.isEmpty() || m_selectedTextFieldId == fieldId
        || rowForFieldId(fieldId) < 0) {
        return;
    }

    m_selectedTextFieldId = fieldId;
    emit textFieldSelectionChanged();
}

void PDFFormDesignerController::clearTextFieldSelection()
{
    if (m_selectedTextFieldId.isEmpty()) {
        return;
    }

    m_selectedTextFieldId.clear();
    emit textFieldSelectionChanged();
}

void PDFFormDesignerController::setTextFieldFocused(const QString &fieldId,
                                                    bool focused)
{
    if (focused) {
        if (fieldId.isEmpty() || m_focusedTextFieldId == fieldId
            || rowForFieldId(fieldId) < 0) {
            return;
        }

        m_focusedTextFieldId = fieldId;
        emit textFieldFocusedChanged();
        return;
    }

    if (m_focusedTextFieldId != fieldId) {
        return;
    }
    clearTextFieldFocus();
}

void PDFFormDesignerController::clear()
{
    ++m_sourceReloadSerial;
    clearTextFieldFocus();
    clearTextFieldSelection();
    const bool hadFields = !m_fields.isEmpty();
    const bool hadRemovedSourceFields = !m_removedSourceFields.isEmpty();
    if (!hadFields && !hadRemovedSourceFields) {
        m_nextFieldId = 0;
        return;
    }

    if (hadFields) {
        beginRemoveRows({}, 0, m_fields.size() - 1);
        m_fields.clear();
        endRemoveRows();
    }
    m_removedSourceFields.clear();
    m_nextFieldId = 0;
    emit textFieldsChanged(-1);
    emit draftStateChanged();
}

void PDFFormDesignerController::resetFromDocument()
{
    reloadSourceFields();
}

bool PDFFormDesignerController::hasDrafts() const
{
    return !m_removedSourceFields.isEmpty()
        || std::any_of(
            m_fields.cbegin(),
            m_fields.cend(),
            [](const FormField &field) {
                return field.changedProperties != NoPropertyChanges;
            });
}

QVector<PDFFormDesignerController::SaveField>
PDFFormDesignerController::saveFields(
    bool includeAppearanceNormalization,
    const QSet<int> &nativeAppearancePages,
    bool normalizeAllNativeAppearances) const
{
    QVector<SaveField> fields;
    fields.reserve(m_fields.size() + m_removedSourceFields.size());
    QSet<int> normalizationPages;
    if (includeAppearanceNormalization) {
        // Normalize legacy peers only on pages where the user made a real
        // form edit. This avoids rewriting every Widget in a large document.
        for (const FormField &field : m_fields) {
            if (field.changedProperties == NoPropertyChanges) {
                continue;
            }
            const int page = displayPageForField(field);
            if (page >= 0) {
                normalizationPages.insert(page);
            }
        }
    }
    for (const FormField &field : m_fields) {
        const int page = displayPageForField(field);
        const int currentAppearanceVersion
            = field.type == Text
            ? PDFFormAppearance::kTextFieldVersion
            : field.type == Dropdown
                ? PDFFormAppearance::kDropdownVersion
                : 0;
        const bool normalizeLegacyAppearance
            = includeAppearanceNormalization
            && field.sourceBacked
            && (field.type == Text || field.type == Dropdown)
            && field.appearanceVersion
                < currentAppearanceVersion
            && normalizationPages.contains(page);
        const bool normalizeNativeAppearance
            = field.sourceBacked
            && (field.type == Text || field.type == Dropdown)
            && (normalizeAllNativeAppearances
                || nativeAppearancePages.contains(page));
        const bool normalizeAppearance
            = normalizeLegacyAppearance || normalizeNativeAppearance;
        if (field.changedProperties == NoPropertyChanges
            && !normalizeAppearance) {
            continue;
        }
        if (page < 0 || field.normalizedRect.isEmpty()) {
            continue;
        }
        SaveField saved;
        saved.id = field.id;
        saved.name = field.name;
        saved.tooltip = field.tooltip;
        saved.value = field.value;
        saved.defaultValue = field.sourceBacked
            ? field.defaultValue
            : field.type == Checkbox || field.type == Dropdown
                ? field.defaultValue
                : field.value;
        saved.options = field.options;
        saved.type = field.type;
        saved.page = page;
        saved.normalizedRect = field.normalizedRect;
        saved.flags = field.flags;
        saved.operation = field.sourceBacked
            ? SaveField::Update
            : SaveField::Add;
        saved.changedProperties = field.changedProperties;
        fields.append(std::move(saved));
    }
    for (const FormField &field : m_removedSourceFields) {
        const int page = displayPageForField(field);
        SaveField saved;
        saved.id = field.id;
        saved.name = field.name;
        saved.tooltip = field.tooltip;
        saved.value = field.value;
        saved.defaultValue = field.defaultValue;
        saved.options = field.options;
        saved.type = field.type;
        saved.page = page;
        saved.normalizedRect = field.sourceNormalizedRect;
        saved.flags = field.flags;
        saved.operation = SaveField::Remove;
        saved.changedProperties = NoPropertyChanges;
        fields.append(std::move(saved));
    }
    return fields;
}

int PDFFormDesignerController::rowForFieldId(const QString &fieldId) const
{
    const auto field = std::find_if(
        m_fields.cbegin(),
        m_fields.cend(),
        [&fieldId](const FormField &candidate) {
            return candidate.id == fieldId;
        });
    return field == m_fields.cend()
        ? -1
        : static_cast<int>(std::distance(m_fields.cbegin(), field));
}

int PDFFormDesignerController::displayPageForField(const FormField &field) const
{
    if (field.hasSourcePage && m_document) {
        return m_document->displayPageIndexForSourcePage(field.sourcePage);
    }
    return field.page;
}

QVariantMap PDFFormDesignerController::fieldToMap(const FormField &field) const
{
    QVariantMap value;
    value.insert(QStringLiteral("id"), field.id);
    value.insert(QStringLiteral("type"), field.type);
    value.insert(QStringLiteral("page"), displayPageForField(field));
    value.insert(QStringLiteral("sourcePage"), field.sourcePage);
    value.insert(QStringLiteral("hasSourcePage"), field.hasSourcePage);
    value.insert(QStringLiteral("rect"), field.normalizedRect);
    value.insert(QStringLiteral("x"), field.normalizedRect.x());
    value.insert(QStringLiteral("y"), field.normalizedRect.y());
    value.insert(QStringLiteral("width"), field.normalizedRect.width());
    value.insert(QStringLiteral("height"), field.normalizedRect.height());
    value.insert(QStringLiteral("value"), field.value);
    value.insert(QStringLiteral("name"), field.name);
    value.insert(QStringLiteral("options"), field.options);
    value.insert(QStringLiteral("flags"), field.flags);
    value.insert(
        QStringLiteral("dirty"),
        field.changedProperties != NoPropertyChanges);
    value.insert(
        QStringLiteral("optionsDirty"),
        bool(field.changedProperties & OptionsPropertyChanged));
    value.insert(QStringLiteral("sourceBacked"), field.sourceBacked);
    return value;
}

void PDFFormDesignerController::reloadSourceFields()
{
    ++m_sourceValueRefreshSerial;
    m_sourceValueRefreshPending = false;
    m_sourceValueRefreshQueued = false;
    const quint64 requestSerial = ++m_sourceReloadSerial;
    clearTextFieldFocus();
    clearTextFieldSelection();
    const bool hadFields = !m_fields.isEmpty();
    const bool hadRemovedSourceFields = !m_removedSourceFields.isEmpty();
    if (!m_fields.isEmpty()) {
        beginRemoveRows({}, 0, m_fields.size() - 1);
        m_fields.clear();
        endRemoveRows();
    }
    m_removedSourceFields.clear();
    if (hadFields || hadRemovedSourceFields) {
        emit textFieldsChanged(-1);
        emit draftStateChanged();
    }
    m_nextFieldId = 0;

    if (!m_document
        || !m_document->isLoaded()
        || !m_formController
        || m_formController->document() != m_document) {
        return;
    }

    QPointer<PDFFormDesignerController> controller(this);
    QPointer<PDFDocument> document(m_document);
    m_formController->requestTrePDFTextFields(
        [controller, document, requestSerial](
            bool succeeded,
            QVector<PDFFormWorker::TrePDFTextField> sourceFields) {
            if (!controller
                || !document
                || controller->m_document != document
                || controller->m_sourceReloadSerial != requestSerial
                || !succeeded
                || sourceFields.isEmpty()) {
                return;
            }

            QVector<FormField> hydratedFields;
            hydratedFields.reserve(sourceFields.size());
            QSet<QString> fieldIds;
            for (const PDFFormWorker::TrePDFTextField &source : sourceFields) {
                if (source.id.isEmpty()
                    || fieldIds.contains(source.id)
                    || (source.type != FPDF_FORMFIELD_TEXTFIELD
                        && source.type != FPDF_FORMFIELD_CHECKBOX
                        && source.type != FPDF_FORMFIELD_COMBOBOX)
                    || source.sourcePage < 0
                    || source.normalizedRect.isEmpty()
                    || document->displayPageIndexForSourcePage(
                           source.sourcePage) < 0) {
                    continue;
                }
                fieldIds.insert(source.id);
                FormField field;
                field.id = source.id;
                field.type = source.type == FPDF_FORMFIELD_CHECKBOX
                    ? Checkbox
                    : source.type == FPDF_FORMFIELD_COMBOBOX
                        ? Dropdown
                        : Text;
                field.page = document->displayPageIndexForSourcePage(
                    source.sourcePage);
                field.sourcePage = source.sourcePage;
                field.sourceAnnotationIndex = source.annotationIndex;
                field.hasSourcePage = true;
                field.normalizedRect = source.normalizedRect;
                field.sourceNormalizedRect = source.normalizedRect;
                field.value = source.value;
                field.name = source.name;
                field.tooltip = source.tooltip;
                field.defaultValue = source.defaultValue;
                field.options = source.options;
                field.sourceIdentityToken = source.identityToken;
                field.appearanceVersion = source.appearanceVersion;
                field.flags = (source.flags & FPDF_FORMFLAG_REQUIRED
                                   ? Required
                                   : NoFieldFlags)
                    | (source.flags & FPDF_FORMFLAG_READONLY
                           ? ReadOnly
                           : NoFieldFlags);
                field.sourceBacked = true;
                field.changedProperties = NoPropertyChanges;
                hydratedFields.append(std::move(field));
            }
            if (hydratedFields.isEmpty()) {
                return;
            }

            controller->beginInsertRows(
                {},
                controller->m_fields.size(),
                controller->m_fields.size() + hydratedFields.size() - 1);
            controller->m_fields += std::move(hydratedFields);
            controller->endInsertRows();
            emit controller->textFieldsChanged(-1);
        });
}

void PDFFormDesignerController::handlePageStructureChanged()
{
    if (m_fields.isEmpty() && m_removedSourceFields.isEmpty()) {
        return;
    }

    // Source pages have a stable PDF page index and are remapped on demand.
    // Transient blank pages do not currently expose a stable identity, so a
    // later page-structure edit must drop fields on those pages instead of
    // allowing them to appear on unrelated content.
    bool fieldsRemoved = false;
    for (int row = m_fields.size() - 1; row >= 0; --row) {
        if (m_fields.at(row).hasSourcePage) {
            continue;
        }
        const QString fieldId = m_fields.at(row).id;
        if (m_focusedTextFieldId == fieldId) {
            clearTextFieldFocus();
        }
        if (m_selectedTextFieldId == fieldId) {
            clearTextFieldSelection();
        }
        beginRemoveRows({}, row, row);
        m_fields.removeAt(row);
        endRemoveRows();
        fieldsRemoved = true;
    }

    if (textFieldFocused()) {
        const int focusedRow = rowForFieldId(m_focusedTextFieldId);
        if (focusedRow < 0 || displayPageForField(m_fields.at(focusedRow)) < 0) {
            clearTextFieldFocus();
        }
    }
    if (!m_selectedTextFieldId.isEmpty()) {
        const int selectedRow = rowForFieldId(m_selectedTextFieldId);
        if (selectedRow < 0 || displayPageForField(m_fields.at(selectedRow)) < 0) {
            clearTextFieldSelection();
        }
    }

    if (!m_fields.isEmpty()) {
        emit dataChanged(index(0), index(m_fields.size() - 1), {PageRole});
    }
    emit textFieldsChanged(-1);
    if (fieldsRemoved) {
        emit draftStateChanged();
    }
}

void PDFFormDesignerController::clearTextFieldFocus()
{
    if (m_focusedTextFieldId.isEmpty()) {
        return;
    }

    m_focusedTextFieldId.clear();
    emit textFieldFocusedChanged();
}
