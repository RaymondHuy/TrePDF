#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class PDFDocument;
class PDFFormController;

class PDFFormDesignerController : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool textFieldFocused READ textFieldFocused
                   NOTIFY textFieldFocusedChanged FINAL)
    Q_PROPERTY(QString focusedTextFieldId READ focusedTextFieldId
                   NOTIFY textFieldFocusedChanged FINAL)
    Q_PROPERTY(QString selectedTextFieldId READ selectedTextFieldId
                   NOTIFY textFieldSelectionChanged FINAL)
    Q_PROPERTY(QString selectedFieldId READ selectedTextFieldId
                   NOTIFY textFieldSelectionChanged FINAL)

public:
    enum FieldType {
        Text = 0,
        Checkbox,
        RadioButton,
        Dropdown,
        Date,
        Signature
    };
    Q_ENUM(FieldType)

    enum FieldFlag {
        NoFieldFlags = 0,
        Required = 1 << 0,
        ReadOnly = 1 << 1
    };
    Q_ENUM(FieldFlag)

    enum ChangedProperty : quint32 {
        NoPropertyChanges = 0,
        ValuePropertyChanged = 1u << 0,
        NamePropertyChanged = 1u << 1,
        OptionsPropertyChanged = 1u << 2,
        FlagsPropertyChanged = 1u << 3,
        RectPropertyChanged = 1u << 4,
        AllPropertiesChanged = ValuePropertyChanged
            | NamePropertyChanged
            | OptionsPropertyChanged
            | FlagsPropertyChanged
            | RectPropertyChanged
    };

    enum Role {
        IdRole = Qt::UserRole + 1,
        TypeRole,
        PageRole,
        SourcePageRole,
        HasSourcePageRole,
        RectRole,
        XRole,
        YRole,
        WidthRole,
        HeightRole,
        ValueRole,
        NameRole,
        OptionsRole,
        FlagsRole,
        DirtyRole,
        SourceBackedRole
    };
    Q_ENUM(Role)

    struct SaveField
    {
        enum Operation {
            Add,
            Update,
            Remove
        };

        QString id;
        QString name;
        QString tooltip;
        QString value;
        QString defaultValue;
        QStringList options;
        FieldType type = Text;
        int page = -1;
        QRectF normalizedRect;
        int flags = NoFieldFlags;
        Operation operation = Add;
        quint32 changedProperties = AllPropertiesChanged;
    };

    explicit PDFFormDesignerController(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool textFieldFocused() const;
    QString focusedTextFieldId() const;
    QString selectedTextFieldId() const;

    void setFormController(PDFFormController *formController);
    Q_INVOKABLE QString addField(int type,
                                 int page,
                                 qreal x,
                                 qreal y,
                                 qreal width,
                                 qreal height);
    Q_INVOKABLE QString addTextField(int page,
                                     qreal x,
                                     qreal y,
                                     qreal width,
                                     qreal height);
    Q_INVOKABLE QVariantMap fieldData(const QString &fieldId) const;
    Q_INVOKABLE bool activateSourceCheckbox(const QString &fieldId,
                                            qreal viewWidth,
                                            qreal viewHeight);
    Q_INVOKABLE bool activateSourceDropdown(const QString &fieldId,
                                            int optionIndex);
    Q_INVOKABLE QVariantList fieldsForPage(int page) const;
    Q_INVOKABLE QVariantList textFieldsForPage(int page) const;
    QStringList sourceFieldIdsForPage(int page) const;
    bool managesSourceChoiceField(int sourcePage,
                                  int annotationIndex,
                                  const QString &identityToken) const;
    bool managesSourceWidget(int page,
                             int annotationIndex,
                             const QString &widgetId) const;
    bool sourceFieldContainsPoint(
        int page,
        const QPointF &normalizedPoint) const;
    QStringList sourceTextFieldIdsForPage(int page) const;
    bool sourceTextFieldContainsPoint(
        int page,
        const QPointF &normalizedPoint) const;
    Q_INVOKABLE bool setFieldValue(const QString &fieldId,
                                   const QString &value);
    Q_INVOKABLE bool setFieldName(const QString &fieldId,
                                  const QString &name);
    Q_INVOKABLE bool setFieldOptions(const QString &fieldId,
                                     const QStringList &options);
    Q_INVOKABLE bool renameFieldOption(const QString &fieldId,
                                       int optionIndex,
                                       const QString &option);
    Q_INVOKABLE bool setFieldFlags(const QString &fieldId, int flags);
    Q_INVOKABLE void setTextFieldText(const QString &fieldId,
                                      const QString &text);
    Q_INVOKABLE bool setFieldRect(const QString &fieldId,
                                  qreal x,
                                  qreal y,
                                  qreal width,
                                  qreal height);
    Q_INVOKABLE bool moveField(const QString &fieldId,
                              qreal x,
                              qreal y);
    Q_INVOKABLE bool setTextFieldRect(const QString &fieldId,
                                      qreal x,
                                      qreal y,
                                      qreal width,
                                      qreal height);
    Q_INVOKABLE bool removeField(const QString &fieldId);
    Q_INVOKABLE bool removeTextField(const QString &fieldId);
    Q_INVOKABLE void selectTextField(const QString &fieldId);
    Q_INVOKABLE void clearTextFieldSelection();
    Q_INVOKABLE void setTextFieldFocused(const QString &fieldId, bool focused);
    Q_INVOKABLE void clearTextFieldFocus();
    Q_INVOKABLE void prepareForDesignMode();

    void setDocument(PDFDocument *document);
    void refreshSourceFieldValues();
    bool hasDrafts() const;
    QVector<SaveField> saveFields(
        bool includeAppearanceNormalization = false,
        const QSet<int> &nativeAppearancePages = {},
        bool normalizeAllNativeAppearances = false) const;
    void clear();
    void resetFromDocument();

signals:
    void textFieldsChanged(int page);
    void textFieldFocusedChanged();
    void textFieldSelectionChanged();
    void draftStateChanged();

private:
    struct FormField
    {
        QString id;
        FieldType type = Text;
        int page = -1;
        int sourcePage = -1;
        int sourceAnnotationIndex = -1;
        bool hasSourcePage = false;
        QRectF normalizedRect;
        QRectF sourceNormalizedRect;
        QString value;
        QString name;
        QString sourceRadioGroupName;
        QString tooltip;
        QString defaultValue;
        QStringList options;
        QString sourceIdentityToken;
        int flags = NoFieldFlags;
        int appearanceVersion = 0;
        bool sourceBacked = false;
        quint32 changedProperties = AllPropertiesChanged;
    };

    int rowForFieldId(const QString &fieldId) const;
    int displayPageForField(const FormField &field) const;
    QVariantMap fieldToMap(const FormField &field) const;
    void handlePageStructureChanged();
    void reloadSourceFields();

    QPointer<PDFDocument> m_document;
    QPointer<PDFFormController> m_formController;
    QVector<FormField> m_fields;
    QVector<FormField> m_removedSourceFields;
    QString m_focusedTextFieldId;
    QString m_selectedTextFieldId;
    quint64 m_nextFieldId = 0;
    quint64 m_sourceReloadSerial = 0;
    quint64 m_sourceValueRefreshSerial = 0;
    bool m_sourceValueRefreshPending = false;
    bool m_sourceValueRefreshQueued = false;
};
