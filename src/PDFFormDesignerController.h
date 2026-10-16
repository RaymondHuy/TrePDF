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

/**
 * @brief QML-facing list model for form-design drafts and supported saved widgets.
 *
 * Rows combine newly placed fields with supported source fields rehydrated through
 * PDFFormController. Geometry uses top-left normalized visible-page coordinates:
 * x/width are fractions of page width and y/height fractions of page height, not
 * PDF points or screen pixels. Display-page indices follow current page structure;
 * sourcePage retains the saved source-page identity when available.
 *
 * Designer setters change model snapshots and changed-property masks. They do not
 * write a PDF, enqueue their own Undo/Redo commands, or automatically mirror every
 * change into the native PDFium form session. Conversely, source activation APIs
 * dispatch native form interaction and refresh values asynchronously; those changes
 * belong to the form controller's native state rather than this model's dirty mask.
 * The surrounding session/writer combines these sources when guarding or saving.
 *
 * Use this QObject/model on its owning GUI thread. Attached document/controller
 * pointers are borrowed and guarded by QPointer; the class does not own them or
 * expose live PDFium handles. Worker snapshots return through guarded callbacks.
 * Switching dependencies, reloading, or resetting can discard designer drafts;
 * callers must resolve any user-facing save/discard decision before those actions.
 *
 * Historical TextField names also cover other field types in several APIs and
 * signals; each method documents its actual filtering. FieldFlag values are this
 * controller's flags, not raw PDF /Ff bits. A model row or accepted setter value
 * does not by itself establish that the final writer supports that operation.
 */
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
    Q_PROPERTY(int maximumTextFieldInputLength
                   READ maximumTextFieldInputLength CONSTANT FINAL)

public:
    enum FieldType {
        Text = 0,
        Checkbox,
        RadioButton,
        Dropdown,
        Date,
        Signature,
        ListBox
    };
    Q_ENUM(FieldType)

    enum FieldFlag {
        NoFieldFlags = 0,
        Required = 1 << 0,
        ReadOnly = 1 << 1,
        Multiline = 1 << 2,
        Comb = 1 << 3
    };
    Q_ENUM(FieldFlag)

    enum ChangedProperty : quint32 {
        NoPropertyChanges = 0,
        ValuePropertyChanged = 1u << 0,
        NamePropertyChanged = 1u << 1,
        OptionsPropertyChanged = 1u << 2,
        FlagsPropertyChanged = 1u << 3,
        RectPropertyChanged = 1u << 4,
        MaxLengthPropertyChanged = 1u << 5,
        AllPropertiesChanged = ValuePropertyChanged
            | NamePropertyChanged
            | OptionsPropertyChanged
            | FlagsPropertyChanged
            | RectPropertyChanged
            | MaxLengthPropertyChanged
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
        SourceBackedRole,
        MaxLengthRole,
        SelectedValuesRole
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
        QStringList selectedValues;
        FieldType type = Text;
        int page = -1;
        QRectF normalizedRect;
        int flags = NoFieldFlags;
        int maxLength = 0;
        Operation operation = Add;
        quint32 changedProperties = AllPropertiesChanged;
    };

    /**
     * @brief Creates an empty list model without an attached document or form worker.
     * @param parent Optional QObject owner; ordinary Qt parent ownership applies.
     * No source scan or PDF mutation occurs during construction. Focus/selection
     * start empty; setDocument() and setFormController() establish the dependencies
     * needed to load source widgets. Standalone draft rows can be added beforehand.
     */
    explicit PDFFormDesignerController(QObject *parent = nullptr);

    /**
     * @brief Returns the number of active field rows in this flat model.
     * @param parent Invalid for the root list; any valid parent has zero children.
     * @return Active row count at the root, including rows whose source pages may
     *         currently be hidden/deleted. Pending source removals are stored
     *         separately and are not counted. This is not a per-page count.
     */
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    /**
     * @brief Reads one field role without changing the row or consulting PDFium.
     * @param index Model index selecting an active row. Invalid/out-of-range rows
     *        return an invalid QVariant; callers should supply this model's index.
     * @param role One of Role, or Qt::DisplayRole, which aliases ValueRole.
     * @return The requested snapshot value, or invalid QVariant for an unknown role.
     * PageRole is resolved against current page structure; rectangle/component
     * roles remain normalized. DirtyRole tests changedProperties, independently of
     * native form edits. SourceBackedRole differs from HasSourcePageRole: a new
     * draft placed on an existing source page can have only the latter set.
     */
    QVariant data(const QModelIndex &index, int role) const override;

    /**
     * @brief Supplies the stable QML names for the custom model roles.
     * @return Mapping for id, type, page, sourcePage, hasSourcePage, rect, x, y,
     *         width, height, value, name, options, flags, dirty, sourceBacked,
     *         maxLength, and selectedValues. Values come from data(); this method
     *         only names the roles and does not load or refresh fields.
     */
    QHash<int, QByteArray> roleNames() const override;

    /**
     * @brief Reports whether the controller currently records a focused field ID.
     * @return True exactly when focusedTextFieldId() is nonempty. This is designer
     *         bookkeeping, not a query of native PDFium or platform keyboard focus.
     * @see setTextFieldFocused()
     */
    bool textFieldFocused() const;

    /**
     * @brief Returns the field ID tracked as focused by the designer.
     * @return Stored ID, or empty when focus is clear. Despite the method name,
     *         setTextFieldFocused() does not restrict the ID to a Text row.
     * Changes are reported by textFieldFocusedChanged(); no focus is moved here.
     */
    QString focusedTextFieldId() const;

    /**
     * @brief Returns the designer's selected field ID for any supported row type.
     * @return Stored selection, or empty. Both QML selectedTextFieldId and
     *         selectedFieldId properties use this getter; selection is independent
     *         of tracked focus and does not itself create a draft change.
     */
    QString selectedTextFieldId() const;

    /**
     * @brief Returns the global cap used for interactive Text field input.
     * @return 300000 UTF-16 code units, not bytes or grapheme clusters. A positive
     *         per-field maxLength can impose a smaller cap. Input truncation avoids
     *         splitting a surrogate pair. This constant is not a blanket limit
     *         applied to every imported source value or every non-Text field type.
     */
    int maximumTextFieldInputLength() const;

    /**
     * @brief Attaches the borrowed native form controller and reloads source rows.
     * @param formController Controller used for source snapshots/native activation,
     *        or nullptr to detach. Ownership is not transferred; QPointer observes
     *        destruction, which clears the pointer and triggers another reload.
     * The same pointer is a no-op. Otherwise old signal connections to this model
     * are disconnected and reloadSourceFields() runs, immediately discarding current
     * designer rows/removals before any asynchronous rehydration. Loading requires
     * this controller and the attached PDFDocument to refer to the same document.
     * @warning Resolve pending designer edits before changing this dependency.
     */
    void setFormController(PDFFormController *formController);
    /**
     * @brief Appends a new, unsaved field with generated identity and default data.
     *
     * Accepts enum values Text through ListBox, including the declared Date and
     * Signature types; acceptance here does not guarantee writer support. A UUID
     * supplies the persistent-style ID suffix, while the generated name also uses
     * an incrementing local sequence. A source-page identity is captured when the
     * attached document can map the supplied display page; sourceBacked stays false.
     *
     * Checkbox/RadioButton start Off with Yes/Off options. Dropdown starts with three
     * localized options, ListBox with five and the first selected. Other types start
     * empty. The new row has all properties marked changed. Emits row insertion,
     * textFieldsChanged(page), and draftStateChanged(), without selecting/focusing
     * the field, saving it, or creating an Undo command.
     *
     * @param type FieldType integer; values outside the enum range are rejected.
     * @param page Nonnegative zero-based display page. This method does not require
     *        a loaded document or validate page against its current page count.
     * @param x Normalized horizontal coordinate.
     * @param y Normalized vertical coordinate, measured from the visible page top.
     * @param width Normalized width; negative extents are normalized first.
     * @param height Normalized height; the rectangle's edges are clipped to [0, 1].
     * @return New ID, or empty for an invalid type/page or an empty clipped rectangle.
     * @pre Supply finite geometry; the bounding helper is not a dedicated NaN/Inf
     *      validator. Final page/field eligibility is checked by the caller/writer.
     */
    Q_INVOKABLE QString addField(int type,
                                 int page,
                                 qreal x,
                                 qreal y,
                                 qreal width,
                                 qreal height);
    /**
     * @brief Convenience wrapper for addField(Text, ...).
     * @param page Zero-based display page; must be nonnegative.
     * @param x Normalized left coordinate.
     * @param y Normalized top coordinate.
     * @param width Normalized horizontal extent.
     * @param height Normalized vertical extent.
     * @return New field ID or empty, with the same clipping, defaults, model signals,
     *         and caller preconditions as addField(). Does not start inline input.
     */
    Q_INVOKABLE QString addTextField(int page,
                                     qreal x,
                                     qreal y,
                                     qreal width,
                                     qreal height);
    /**
     * @brief Adds a Text field and enables its comb layout through setFieldFlags().
     * A new comb field uses the default five-character limit and cannot be multiline.
     * Creation and flag application use their normal model/draft notifications;
     * they are two calls, not a separately rollback-protected compound transaction.
     * @param page Nonnegative display-page index.
     * @param x Normalized left coordinate.
     * @param y Normalized top coordinate.
     * @param width Normalized horizontal extent.
     * @param height Normalized vertical extent.
     * @return ID after successful creation and flag application, otherwise empty.
     *         If the second step were to fail, this wrapper does not remove the row.
     * @see addField()
     */
    Q_INVOKABLE QString addCombField(int page,
                                     qreal x,
                                     qreal y,
                                     qreal width,
                                     qreal height);
    /**
     * @brief Returns a QML snapshot of the first active field matching an exact ID.
     * @param fieldId Case-sensitive field ID; removed-source records are not searched.
     * @return fieldToMap() data, or an empty map when no active row matches. Geometry
     *         reflects the current draft, page is mapped to the current display
     *         order, and dirty/optionsDirty describe designer property masks.
     * The map is a detached value; editing it does not mutate this model or PDFium.
     */
    Q_INVOKABLE QVariantMap fieldData(const QString &fieldId) const;

    /**
     * @brief Dispatches native activation for a saved checkbox or radio button.
     * Requires an active source-backed Checkbox/RadioButton row, a loaded matching
     * document/form controller, interactivity, a surviving display page, positive
     * view dimensions, and no ReadOnly flag. Uses the center of the original source
     * rectangle, scaled into the provided view, to send native Press and Release;
     * a draft move/resize does not change that native hit target. It then kills
     * native focus and requests an asynchronous value refresh.
     * @param fieldId ID of the source-backed button to activate; despite the method
     *        name, RadioButton is also accepted.
     * @param viewWidth Width of the page view used for normalized-to-device mapping.
     * @param viewHeight Height of that view. Native device extents are rounded and
     *        clamped to at least one; callers should provide finite usable sizes.
     * @return True when interaction was dispatched, not proof that PDFium changed
     *         the value. False when a prerequisite fails. No designer Undo command
     *         or dirty bit is created directly; native state is owned elsewhere.
     */
    Q_INVOKABLE bool activateSourceCheckbox(const QString &fieldId,
                                            qreal viewWidth,
                                            qreal viewHeight);
    /**
     * @brief Compatibility alias for activateSourceChoice().
     * @param fieldId Source choice-field ID. The implementation also accepts ListBox;
     *        the legacy Dropdown name does not impose an extra type restriction.
     * @param optionIndex Zero-based option to select.
     * @return Whether native selection was dispatched, with the same validation,
     *         asynchronous refresh, and completion limitations as activateSourceChoice().
     */
    Q_INVOKABLE bool activateSourceDropdown(const QString &fieldId,
                                            int optionIndex);
    /**
     * @brief Requests native selection of one option in a saved Dropdown/ListBox.
     * Delegates to changeSourceChoiceOption(..., false). Requires a current source
     * annotation index/identity token, a valid option, a loaded matching document,
     * an interactive form controller, a visible page mapping, and no ReadOnly flag.
     * The worker receives the identity-checked selection request; local values are
     * subsequently refreshed rather than changed optimistically in this method.
     * @param fieldId Active source-backed choice-field ID.
     * @param optionIndex Zero-based index in the row's current options snapshot.
     * @return True for a dispatched request; false for invalid/ineligible input.
     *         True is not synchronous confirmation of the final native selection.
     */
    Q_INVOKABLE bool activateSourceChoice(const QString &fieldId,
                                          int optionIndex);
    /**
     * @brief Requests native toggling of one option in a saved multi-select ListBox.
     * Delegates to changeSourceChoiceOption(..., true), which adds a ListBox-only
     * requirement to the ordinary source-choice identity/interactivity checks.
     * This targets the native form session, not setFieldSelectedValues()'s designer
     * draft path. Refresh is asynchronous and no local undo entry is created here.
     * @param fieldId Active source-backed ListBox ID.
     * @param optionIndex Zero-based option to toggle through the worker.
     * @return True if dispatched, false if prerequisites fail; not a completion result.
     */
    Q_INVOKABLE bool toggleSourceChoiceOption(const QString &fieldId,
                                              int optionIndex);
    /**
     * @brief Returns fieldToMap() snapshots for active rows on a display page.
     * @param page Current zero-based display-page index, compared with each row's
     *        mapped page. No separate range check is performed, so passing -1 may
     *        match rows whose source pages no longer have a display mapping.
     * @return Maps in model order, including new and source-backed fields of every
     *         type, excluding pending removals. Does not scan or refresh the PDF.
     */
    Q_INVOKABLE QVariantList fieldsForPage(int page) const;

    /**
     * @brief Returns active Text rows on a display page, with a legacy text alias.
     * @param page Mapped display-page value to match; uses the same equality-based
     *        filtering as fieldsForPage() without a separate bounds check.
     * @return Text-only maps in model order, each adding "text" equal to "value".
     *         Comb fields qualify because they are Text rows with a flag. Removed
     *         records and non-Text field types are excluded.
     */
    Q_INVOKABLE QVariantList textFieldsForPage(int page) const;

    /**
     * @brief Lists managed source IDs whose saved widgets may need overlay handling.
     * @param page Current display-page value used to map each source record.
     * @return IDs of source-backed active rows followed by matching pending source
     *         removals. Including removals lets consumers continue identifying the
     *         saved widget while its deletion is only a draft. New draft fields on
     *         source pages are excluded because they are not source-backed widgets.
     */
    QStringList sourceFieldIdsForPage(int page) const;

    /**
     * @brief Checks ownership of an exact source choice-widget identity snapshot.
     * Searches active and pending-removed source-backed Dropdown/ListBox records.
     * It checks stored identity only, not PDFium's current annotation dictionaries.
     * @param sourcePage Original saved-page index, not the current display index.
     * @param annotationIndex Saved annotation index; must be nonnegative.
     * @param identityToken Nonempty token supplied by the source-field scan.
     * @return True for an exact triple/type match; false for invalid inputs or no
     *         managed record. Pending removal still counts as managed ownership.
     */
    bool managesSourceChoiceField(int sourcePage,
                                  int annotationIndex,
                                  const QString &identityToken) const;
    /**
     * @brief Checks whether a saved widget belongs to this designer's managed set.
     * @param page Nonnegative current display-page index, unlike the source-page
     *        parameter of managesSourceChoiceField().
     * @param annotationIndex Nonnegative stored source annotation index.
     * @param widgetId Nonempty exact model/widget ID.
     * @return True for a source-backed active or pending-removed record matching all
     *         three values, regardless of field type. This does not revalidate a
     *         fingerprint, source permissions, or the current contents of PDFium.
     */
    bool managesSourceWidget(int page,
                             int annotationIndex,
                             const QString &widgetId) const;
    /**
     * @brief Hit-tests original saved-widget bounds, including pending removals.
     * @param page Nonnegative current display-page index.
     * @param normalizedPoint Finite point in the page's normalized [0, 1] square.
     * @return True if a source-backed active or removed record on that mapped page
     *         contains the point in sourceNormalizedRect. Uses original source
     *         geometry, not the moved/resized draft rectangle. Negative pages or
     *         coordinates outside [0, 1] return false; callers should exclude NaN.
     */
    bool sourceFieldContainsPoint(
        int page,
        const QPointF &normalizedPoint) const;
    /**
     * @brief Lists active source-backed Text IDs on a current display page.
     * @param page Mapped page value to match.
     * @return Matching IDs in model order, including comb Text rows but excluding
     *         pending source removals and all other types. This narrower active-only
     *         behavior differs from sourceFieldIdsForPage(); it is not an alias.
     */
    QStringList sourceTextFieldIdsForPage(int page) const;

    /**
     * @brief Hit-tests original bounds of active source-backed Text widgets only.
     * @param page Nonnegative display-page index.
     * @param normalizedPoint Finite normalized page point within [0, 1].
     * @return True for a containing original source rectangle of an active Text row.
     *         Excludes pending removals, new drafts, and other field types, and does
     *         not follow draft geometry edits. Negative/out-of-range input is
     *         rejected by the same ordinary bounds checks as the broader hit test.
     */
    bool sourceTextFieldContainsPoint(
        int page,
        const QPointF &normalizedPoint) const;
    /**
     * @brief Sets a designer value using the field type's local normalization rules.
     * Text values become single-line unless Multiline is set: CRLF, CR, LF, and
     * Unicode line/paragraph separators become spaces. Text is limited to the
     * smaller of a positive maxLength and 300000 UTF-16 units, or the global limit
     * alone, without splitting a surrogate pair. This is not grapheme-aware clipping.
     * Dropdown values must exactly match an option. ListBox delegates to
     * setFieldSelectedValues(), using a singleton selection or empty selection.
     * RadioButton accepts only Yes/Off; Yes switches same-name radio peers to Off.
     * Other types, including Checkbox, receive the supplied string without those
     * type-specific membership restrictions.
     * @param fieldId Existing active row, either new or source-backed.
     * @param value Requested value; may be normalized/truncated before storage.
     * @return False for unknown IDs or rejected type-specific input; true for an
     *         accepted change or no-op. Changes set ValuePropertyChanged and emit
     *         relevant dataChanged roles plus draftStateChanged(), including peers.
     * @note Does not enforce ReadOnly, change defaultValue, dispatch native input,
     *       save a PDF, or create Undo history; caller/UI eligibility checks are separate.
     */
    Q_INVOKABLE bool setFieldValue(const QString &fieldId,
                                   const QString &value);
    /**
     * @brief Replaces a ListBox's designer selection in canonical option order.
     * Rejects any value not present in options. Removes duplicates and orders the
     * surviving selection by the option list, not by input order. Empty input clears
     * selection; the compatibility value becomes the first selected option or empty.
     * @param fieldId Active ListBox ID; other types are rejected.
     * @param selectedValues Complete desired selection, not a toggle/delta.
     * @return True for an accepted selection, including a canonical no-op; false for
     *         an unknown/non-ListBox row or any unknown option. Changes mark Value
     *         dirty, notify selectedValues/value roles as needed, and emit
     *         draftStateChanged(). No native worker selection is submitted here.
     */
    Q_INVOKABLE bool setFieldSelectedValues(
        const QString &fieldId,
        const QStringList &selectedValues);
    /**
     * @brief Renames a field or moves one radio member into a named group.
     * For RadioButton, requires a nonempty already-trimmed name without '.', and
     * rejects a name occupied by a nonradio row. Joining an existing radio group
     * copies its first peer's flags/tooltip. If the incoming member is selected or
     * selected by default, corresponding target-group peer states are cleared so
     * the merged group remains mutually exclusive. Only this member is renamed.
     * Nonradio names are stored verbatim; flat-name/uniqueness validation belongs
     * to the save pipeline, not this setter. Equality is checked before validation.
     * @param fieldId Active row ID.
     * @param name Exact requested field name/group ID; not automatically trimmed.
     * @return True for unchanged or accepted input; false for an unknown row or
     *         invalid radio-group rename. Accepted changes set the appropriate name,
     *         flags/value dirty bits, notify affected rows, and emit draftStateChanged().
     */
    Q_INVOKABLE bool setFieldName(const QString &fieldId,
                                  const QString &name);
    /**
     * @brief Replaces option data and reconciles choice values with the new list.
     * Dropdown/ListBox lists must contain 1..4096 nonempty, exactly distinct strings
     * totaling at most 1048576 UTF-16 units. Case and whitespace are significant;
     * no trimming occurs. Nonchoice fields also accept/stash option lists here,
     * without applying those choice-specific validation rules.
     * A Dropdown value/default no longer present becomes the first option. ListBox
     * selections are filtered, deduplicated, and reordered by the new options;
     * its value becomes the first selection or empty, while an invalid default
     * becomes the first option. This method does not infer renamed-option identity.
     * @param fieldId Active row ID.
     * @param options Complete replacement list; use renameFieldOption() when one
     *        choice label should retain its selection identity.
     * @return True for accepted/no-op input, false for unknown ID or invalid choice
     *         options. Changes mark Options and any affected Value state dirty,
     *         emit corresponding dataChanged roles, and notify draftStateChanged().
     */
    Q_INVOKABLE bool setFieldOptions(const QString &fieldId,
                                     const QStringList &options);
    /**
     * @brief Renames a single Dropdown/ListBox option while preserving selections.
     * Validates the resulting full list with the same distinctness/size rules as
     * setFieldOptions(). Current/default values and ListBox selections matching the
     * old label follow its new label rather than being discarded. ListBox selection
     * order is then canonicalized. No-op equality succeeds without notifications.
     * @param fieldId Active Dropdown or ListBox ID; nonchoice types are rejected.
     * @param optionIndex Zero-based index of the existing option.
     * @param option Replacement label, stored exactly as supplied if valid.
     * @return True for accepted/no-op rename; false for an unknown/nonchoice row,
     *         invalid index, or invalid replacement list. Actual edits mark and
     *         notify options/value/selection roles as needed and emit draftStateChanged().
     */
    Q_INVOKABLE bool renameFieldOption(const QString &fieldId,
                                       int optionIndex,
                                       const QString &option);
    /**
     * @brief Applies designer flag bits and reconciles Text/group-dependent state.
     * Keeps Required and ReadOnly for all types, plus Multiline and Comb for Text;
     * other bits are masked out. Text cannot have Multiline and Comb together.
     * Enabling Comb supplies maxLength=5 when no positive limit exists. Text values
     * and defaults are re-constrained to the resulting line/length policy.
     * For a RadioButton, the accepted flags apply to every same-name radio member,
     * since one persisted radio group shares field flags.
     * @param fieldId Active row ID.
     * @param flags Bitwise combination of this class's FieldFlag, not PDF /Ff bits.
     * @return True for accepted/no-op flags; false for an unknown row or incompatible
     *         Text flags. Changed rows receive flags/length/value dirty bits and
     *         dataChanged notifications as appropriate, followed by draftStateChanged().
     * @note Setting ReadOnly does not make the other C++ designer setters reject
     *       later calls automatically; native activation/UI checks enforce it separately.
     */
    Q_INVOKABLE bool setFieldFlags(const QString &fieldId, int flags);

    /**
     * @brief Sets a Text field's explicit character limit and constrains its values.
     * @param fieldId Active Text ID; other types and unknown IDs are rejected.
     * @param maxLength Integer in [0, 300000]. Zero removes the explicit PDF /MaxLen
     *        requirement but retains the global interactive cap; it also clears Comb.
     * Current/default values are truncated as needed without splitting surrogate
     * pairs. Limits count UTF-16 units, not user-perceived grapheme clusters.
     * @return True for accepted/no-op input; false for an invalid type/ID/range.
     *         Changes mark MaxLength, Flags, and Value as applicable and emit the
     *         corresponding model roles plus draftStateChanged().
     */
    Q_INVOKABLE bool setFieldMaxLength(const QString &fieldId,
                                       int maxLength);
    /**
     * @brief Compatibility wrapper that forwards to setFieldValue() and ignores its bool.
     * @param fieldId Active field ID; despite this method's name, no Text-only check
     *        is added, so the target type's ordinary setFieldValue() rules apply.
     * @param text Requested value. Rejected input is silently ignored by this void
     *        wrapper; use setFieldValue() when the caller needs an acceptance result.
     */
    Q_INVOKABLE void setTextFieldText(const QString &fieldId,
                                      const QString &text);
    /**
     * @brief Changes a field's normalized draft frame while keeping its source frame.
     * Normalizes negative dimensions, then clips all four edges independently to
     * [0, 1]. Thus clipping can reduce width/height; use moveField() when preserving
     * size is required. An empty clipped rectangle is rejected. The original
     * sourceNormalizedRect remains available for native hit tests and removal.
     * @param fieldId Active field ID of any type.
     * @param x Normalized horizontal origin.
     * @param y Normalized vertical origin.
     * @param width Normalized horizontal extent.
     * @param height Normalized vertical extent.
     * @return True for a changed or identical accepted frame, false for unknown ID
     *         or empty bounds. Changes mark Rect dirty, notify all geometry roles,
     *         textFieldsChanged(mapped page or -1), and draftStateChanged().
     * @pre Supply finite geometry; this method does not explicitly validate NaN/Inf.
     */
    Q_INVOKABLE bool setFieldRect(const QString &fieldId,
                                  qreal x,
                                  qreal y,
                                  qreal width,
                                  qreal height);
    /**
     * @brief Moves a draft frame within the normalized page while preserving its size.
     * @param fieldId Active field ID; unknown rows return false.
     * @param x Requested new left coordinate; must be finite.
     * @param y Requested new top coordinate; must be finite.
     * Coordinates are clamped to [0, max(0, 1 - current dimension)] before delegating
     * to setFieldRect() with the existing width/height. For normally bounded frames
     * this stops at the page edge without shrinking the field.
     * @return The delegated acceptance result, or false for nonfinite coordinates.
     *         Uses the same dirty/model notifications and does not save/native-move.
     */
    Q_INVOKABLE bool moveField(const QString &fieldId,
                              qreal x,
                              qreal y);
    /**
     * @brief Compatibility alias for setFieldRect(), with no Text-only restriction.
     * @param fieldId Active field ID of any type.
     * @param x Normalized horizontal origin.
     * @param y Normalized vertical origin.
     * @param width Normalized width.
     * @param height Normalized height.
     * @return The result of setFieldRect(), including its normalization/clipping,
     *         finite-input precondition, no-op semantics, and notifications.
     */
    Q_INVOKABLE bool setTextFieldRect(const QString &fieldId,
                                      qreal x,
                                      qreal y,
                                      qreal width,
                                      qreal height);
    /**
     * @brief Removes an active draft row or stages deletion of an eligible source widget.
     * New fields of every declared type can be dropped. Source-backed removal is
     * limited to Text, Dropdown, ListBox, and RadioButton; saved Checkbox removal
     * is rejected. An eligible source snapshot is retained in the removed list for
     * save/suppression, while a never-saved draft simply disappears from the model.
     * Matching tracked focus and selection are cleared. Emits row removal,
     * textFieldsChanged(mapped page or -1), and draftStateChanged().
     * @param fieldId Active row to remove; pending-removed records cannot be removed
     *        again through this lookup.
     * @return True after staging/removing the row; false for unknown ID or unsupported
     *         source type. Does not delete the native PDF annotation or add Undo history.
     */
    Q_INVOKABLE bool removeField(const QString &fieldId);

    /**
     * @brief Compatibility alias for removeField(), not a Text-only deletion API.
     * @param fieldId ID of an active row to remove using the general eligibility rules.
     * @return The delegated result, with identical source-removal staging, focus/
     *         selection cleanup, model notifications, and persistence limitations.
     */
    Q_INVOKABLE bool removeTextField(const QString &fieldId);
    /**
     * @brief Selects an existing active row, regardless of its field type.
     * @param fieldId Nonempty row ID. Unknown IDs, empty input, and the already
     *        selected ID are ignored; empty input does not clear selection.
     * Updates only selection bookkeeping and emits textFieldSelectionChanged().
     * Does not focus the row, check ReadOnly, alter values, or create history.
     */
    Q_INVOKABLE void selectTextField(const QString &fieldId);

    /**
     * @brief Clears designer selection if an ID is currently selected.
     * Emits textFieldSelectionChanged() only when the stored selection changes.
     * Leaves tracked focus, field contents, native form state, and drafts intact.
     */
    Q_INVOKABLE void clearTextFieldSelection();

    /**
     * @brief Records or releases the designer's focused row ID.
     * @param fieldId Existing nonempty ID when focused is true; no Text-type or
     *        ReadOnly filter is applied. When focused is false, only a matching
     *        currently focused ID can clear focus, so a stale blur cannot clear
     *        another row's focus.
     * @param focused True to record focus, false to clear that row's recorded focus.
     * Invalid/same-ID focus requests are ignored. Changes emit
     * textFieldFocusedChanged(); they do not move actual QML/native keyboard focus,
     * change selection, commit text, or modify dirty state.
     */
    Q_INVOKABLE void setTextFieldFocused(const QString &fieldId, bool focused);

    /**
     * @brief Clears the tracked focus ID and notifies its dependent QML properties.
     * No-op when already empty; otherwise emits textFieldFocusedChanged(). This is
     * model bookkeeping, not PDFFormController::killFocus(), and does not commit
     * native input or clear selection by itself.
     */
    Q_INVOKABLE void clearTextFieldFocus();

    /**
     * @brief Commits native form focus before refreshing values for design mode.
     * Requires an attached form controller whose document matches this controller's
     * document; otherwise does nothing. Calls its killFocus(), then requests
     * refreshSourceFieldValues() so native edits can reach the designer snapshot.
     * The refresh is asynchronous/coalesced; return does not mean it has finished.
     * This method neither switches the ribbon/tool nor clears designer drafts.
     */
    Q_INVOKABLE void prepareForDesignMode();

    /**
     * @brief Changes the borrowed document and rebuilds the designer's source context.
     * @param document New document, or nullptr to detach; ownership stays with caller.
     * Passing the same pointer does nothing. Otherwise clear() discards current
     * designer state, old document signals are disconnected, and the new document
     * is observed for page structure, source, loaded-state, and destruction changes.
     * Source/loaded changes request a reload; page structure changes remap/drop rows
     * through handlePageStructureChanged(). Destruction clears the guarded pointer.
     * A non-null attachment immediately calls reloadSourceFields(); the method does
     * not wait for the worker result, load a file, or retarget the form controller.
     * @warning The caller must resolve drafts and keep both dependencies aligned.
     */
    void setDocument(PDFDocument *document);

    /**
     * @brief Asynchronously refreshes native values/options without replacing local edits.
     * Requires at least one source-backed row, a loaded document, and a matching
     * form controller. While a request is pending, further calls coalesce into one
     * queued follow-up rather than launching parallel scans. Completion checks
     * guarded object/document pointers, document identity, and the refresh serial;
     * obsolete or failed results are ignored, with a queued retry serviced as needed.
     *
     * Matching source IDs update annotation indices and identity tokens. Values
     * and options refresh only if their corresponding designer property bit is not
     * dirty. ListBox selections are canonicalized against source options. Text is
     * constrained by current single/multiline and explicit maxLength settings; if
     * those constraints change the native value, Value is marked dirty for Save.
     *
     * Emits dataChanged with affected row roles, and draftStateChanged() only when
     * a constrained Text value creates a designer edit. It neither replaces the row
     * set nor performs a save, and return does not mean refresh has completed.
     */
    void refreshSourceFieldValues();

    /**
     * @brief Reports pending designer field changes or source-widget removals.
     * @return True when the removed-source list is nonempty or any active row has
     *         a nonzero changedProperties mask. It does not filter invisible/deleted
     *         pages, inspect native PDFFormController drafts, or count appearance-only
     *         normalization needs. Setting a property back to its source value does
     *         not automatically clear its previously set dirty bit.
     */
    bool hasDrafts() const;

    /**
     * @brief Creates detached writer snapshots for pending field operations and context.
     * New rows become Add, source-backed rows become Update, and saved removal
     * snapshots become Remove. Active rows whose mapped page is negative or whose
     * rectangle is empty are omitted. Removal snapshots are still included when
     * their mapped page is -1 so the downstream writer can handle page deletion;
     * they carry the original source rectangle and NoPropertyChanges.
     *
     * Normally includes changed active fields, plus unchanged radio members needed
     * to rebuild affected source/destination groups. Group-name changes can connect
     * multiple groups, requiring their transitive set of peers; incomplete original
     * group metadata uses the conservative broader radio inclusion path. Thus an
     * Update with NoPropertyChanges can be required context, not a newly dirty row.
     *
     * Added Text/other nonbutton/nonchoice rows use their current value as the saved
     * default. Source fields and added button/choice fields retain their explicit
     * defaultValue. The snapshot also copies identity, names, tooltip, options,
     * canonical selections, controller flags, maximum length, and normalized frame.
     *
     * @param includeAppearanceNormalization Includes eligible old-version source
     *        Text/Dropdown/ListBox/Radio appearances on pages with changed active
     *        fields. A page with only pending removals does not trigger this rule.
     * @param nativeAppearancePages Current display-page indices whose native Text
     *        and choice appearances need synchronization. This can include source
     *        rows independently of includeAppearanceNormalization or dirty bits.
     * @param normalizeAllNativeAppearances Broadens that native Text/choice inclusion
     *        to all applicable pages instead of only nativeAppearancePages.
     * @return Value-owned SaveField list; it may contain clean context/appearance
     *         updates as well as edits. No PDF I/O, asynchronous refresh, validation
     *         error reporting, dirty-bit clearing, or model mutation occurs here.
     * @note This is a snapshot of currently known model state. Commit/refresh pending
     *       input first where needed; the writer still validates supported types,
     *       geometry, names, source identity, and persistence constraints.
     */
    QVector<SaveField> saveFields(
        bool includeAppearanceNormalization = false,
        const QSet<int> &nativeAppearancePages = {},
        bool normalizeAllNativeAppearances = false) const;
    /**
     * @brief Discards designer rows, removal snapshots, focus, and selection.
     * Invalidates outstanding source-reload callbacks and resets the generated-name
     * sequence. Existing rows are removed with model notifications. When there were
     * rows or pending removals, emits textFieldsChanged(-1) and draftStateChanged();
     * an already empty model still resets its counter and tracked focus/selection.
     * Does not detach dependencies, start a reload, reset native form-worker values,
     * or save anything. Use resetFromDocument() to repopulate eligible source rows.
     * @note This is not a substitute for discarding native PDFFormController edits.
     */
    void clear();

    /**
     * @brief Discards designer state and requests a fresh source-field snapshot.
     * Delegates to reloadSourceFields(), which clears immediately and rehydrates
     * asynchronously when document/controller prerequisites hold. Does not directly
     * discard native form state, reopen the PDF, or synchronously guarantee rows
     * are available on return. Use it after the surrounding session has resolved
     * save/discard semantics, not as a way to preserve pending designer changes.
     */
    void resetFromDocument();

signals:
    /**
     * @brief Invalidates page-specific field presentation/geometry snapshots.
     * @param page Affected display page, or -1 for a global refresh/unknown mapping.
     * Emitted for structural/geometry changes and source rehydration; its historical
     * name covers every field type. Value/property edits can instead notify precise
     * model dataChanged roles, so this is not a signal for every possible edit.
     */
    void textFieldsChanged(int page);

    /**
     * @brief Notifies changes to tracked focus, focusedTextFieldId, and textFieldFocused.
     * Reports designer bookkeeping only, not an independent native focus event.
     * No field data or draft change is implied by this notification alone.
     */
    void textFieldFocusedChanged();

    /**
     * @brief Notifies both selectedTextFieldId and selectedFieldId QML properties.
     * Emitted when the stored selection ID changes or is cleared. Selection applies
     * to all field types and is independent of values, dirty masks, and focus.
     */
    void textFieldSelectionChanged();

    /**
     * @brief Signals that consumers should reevaluate designer draft state.
     * Emitted on accepted mutations/clears and constrained source-value updates.
     * It can fire while hasDrafts() remains true (another property changed), rather
     * than only on a boolean transition. It does not represent all native form
     * worker changes, a successful save, or creation of an Undo command.
     */
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
        QStringList selectedValues;
        QString sourceIdentityToken;
        int flags = NoFieldFlags;
        int maxLength = 0;
        int appearanceVersion = 0;
        bool sourceBacked = false;
        quint32 changedProperties = AllPropertiesChanged;
    };

    /**
     * @brief Finds the first active row with an exact, case-sensitive field ID.
     * @param fieldId Identifier to search in m_fields; removals are not inspected.
     * @return Zero-based row or -1. Uses a linear search and does not validate global
     *         uniqueness, access PDFium, or mutate the model.
     */
    int rowForFieldId(const QString &fieldId) const;

    /**
     * @brief Resolves a snapshot's current display page from stable source identity.
     * @param field Active or removed field snapshot.
     * @return document->displayPageIndexForSourcePage(sourcePage) when hasSourcePage
     *         and a document are present, otherwise the stored page value. A deleted
     *         source page can map to -1; this does not itself remove the record.
     */
    int displayPageForField(const FormField &field) const;

    /**
     * @brief Serializes the public designer snapshot used by QML/query consumers.
     * @param field Snapshot to describe; it is not refreshed from native PDFium.
     * @return Map containing id, type, mapped page, sourcePage, hasSourcePage, rect,
     *         x/y/width/height, value, name, options, selectedValues, flags, maxLength,
     *         dirty, optionsDirty, and sourceBacked. Geometry is the current draft
     *         rectangle; dirty flags derive from changedProperties. Internal source
     *         identity/index, original rectangle, defaultValue, tooltip, and appearance
     *         version are intentionally absent. No "text" alias is added here.
     */
    QVariantMap fieldToMap(const FormField &field) const;

    /**
     * @brief Shared validation/dispatch path for native source choice interaction.
     * Requires a loaded matching document and interactive form controller, an active
     * source-backed choice row with stable source page/index/token, a valid option,
     * no ReadOnly flag, and a surviving display-page mapping. Does not directly
     * update designer selection/value bits or record history; dispatch is followed
     * by an asynchronous refresh of source values.
     * @param fieldId Source Dropdown/ListBox ID.
     * @param optionIndex Zero-based option in the current snapshot.
     * @param toggle True for a ListBox-only toggle; false for ordinary selection.
     * @return True when the native request has been handed off, false for failed
     *         prerequisites. Worker completion and final value are not returned.
     */
    bool changeSourceChoiceOption(const QString &fieldId,
                                  int optionIndex,
                                  bool toggle);
    /**
     * @brief Reconciles field presentation after insertion/deletion changes page order.
     * Rows with stable source-page identities remain stored and remap on demand.
     * Active fields on transient blank pages have no such identity and are dropped
     * rather than silently assigned to unrelated content. Pending source-removal
     * snapshots are retained. Focus/selection is cleared if its row is dropped or
     * no longer maps to a display page.
     * Emits PageRole changes for surviving rows and textFieldsChanged(-1);
     * draftStateChanged() is emitted only when transient rows were actually removed.
     * Does nothing when both active and removed lists are empty. This is not a
     * source rescan and does not directly change native form state.
     */
    void handlePageStructureChanged();

    /**
     * @brief Destructively reloads eligible saved widgets through the form worker.
     * Invalidates reload/value-refresh serials, clears pending refresh flags,
     * focus/selection, active/removal records, and the name counter. Initial clearing
     * notifies affected model/draft consumers. If the document is not loaded or the
     * form controller targets another document, the model remains empty.
     *
     * Requests a guarded asynchronous source snapshot. Callbacks require surviving
     * controller/document pointers, matching document identity and serial, and a
     * successful nonempty result. Hydration accepts unique nonempty IDs for native
     * Text/Checkbox/Radio/Combo/List widgets with a valid source page, nonempty frame,
     * and surviving display mapping. It copies original geometry, identity/index,
     * values/options, appearance version, and supported flags into clean source-backed
     * rows. Valid comb state requires positive maxLength and no Multiline flag.
     *
     * Hydrated rows are appended to any new draft rows created while the request
     * was pending; the callback does not replace that newer list. Row insertion and
     * textFieldsChanged(-1) notify presentation, without marking hydration as a draft.
     * Failure/stale completion produces no additional rows or public error result.
     */
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
