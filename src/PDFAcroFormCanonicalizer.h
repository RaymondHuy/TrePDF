#pragma once

#include "PDFCommentAnnotation.h"
#include "PDFWebLinkScanner.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>

namespace ApplicationFonts { struct FontFaceData; }

/**
 * @brief Completes form and annotation dictionaries in a serialized staging PDF.
 *
 * The save pipeline first uses PDFium to create or mark supported objects, then
 * calls these synchronous helpers to write relationships, field structures, and
 * appearances that need explicit PDF dictionary handling. Despite the class name,
 * its responsibilities include ordinary comments and links as well as AcroForm
 * widgets. It does not operate on the GUI document or on live PDFium handles.
 *
 * Nonempty requests require a readable, writable, nonempty intermediate PDF with
 * classic xref tables. The parser follows supported /Prev chains and rejects
 * encryption, xref streams, and hybrid-reference trailers. These helpers are not
 * general repair tools for arbitrary PDFs: the caller must prepare the expected
 * placeholders/markers and validate the source objects' editing eligibility.
 *
 * Each call maps and inspects the file, builds its replacement/new objects in
 * memory, then appends an incremental section containing a new xref and trailer.
 * Original bytes and superseded objects remain in the file. Preparation failures
 * occur before that append; a failed write/flush attempts to truncate the file
 * back to its previous length. This is not an atomic destination-file replacement
 * or a crash-safe transaction. Use a disposable staging file and publish it only
 * after every finalizer and the writer's subsequent validation succeed. A later
 * finalizer's failure does not undo earlier successful calls on the same file.
 *
 * Calls must have exclusive access to their staging file; no file locking,
 * asynchronous scheduling, cancellation token, or progress reporting is provided.
 * An empty request list returns true without opening or validating the file.
 * Success does not clear error, so initialize it before use and use the bool
 * result as the success indicator. Failure diagnostics describe the structural,
 * appearance-generation, or file-I/O problem; they are not stable error codes.
 *
 * Unless stated otherwise, geometry is already converted to PDF page user-space
 * coordinates, in PDF points, excluding the viewer's transient rotation. For a
 * pageRect, QRectF::top() stores the lower PDF y coordinate and bottom() the upper
 * one, so [left, top, right, bottom] becomes the PDF [left, bottom, right, top]
 * rectangle. These methods do not convert normalized editor coordinates.
 */
class PDFAcroFormCanonicalizer
{
public:
    enum class FieldType {
        Text,
        Checkbox,
        RadioButton,
        Dropdown,
        ListBox
    };

    enum class Operation {
        Add,
        Update,
        NormalizeAppearance,
        Remove
    };

    struct Field {
        QString widgetId;
        QString fieldName;
        QString tooltip;
        QString value;
        QStringList selectedValues;
        QString defaultValue;
        QStringList options;
        QRectF pageRect;
        FieldType type = FieldType::Text;
        int fieldFlags = 0;
        int maxLength = 0;
        Operation operation = Operation::Add;
    };

    enum class AnnotationRelationshipKind {
        Reply,
        State
    };

    struct AnnotationRelationship {
        int page = -1;
        QString parentThreadId;
        QString parentAnnotationId;
        QString childAnnotationId;
        AnnotationRelationshipKind kind
            = AnnotationRelationshipKind::Reply;
        QString expectedState;
        QString expectedStateModel;
    };

    struct FreeTextAnnotation {
        int page = -1;
        QString annotationId;
        QString text;
        bool preserveDefaultAppearance = false;
    };

    enum class TextMarkupType {
        Highlight,
        Underline,
        Squiggly,
        StrikeOut
    };

    struct TextMarkupAnnotation {
        int page = -1;
        QString annotationId;
        TextMarkupType type = TextMarkupType::Highlight;
    };

    struct StampAnnotation {
        int page = -1;
        QString annotationId;
        QString stampName;
        QString displayText;
        QString detailText;
        QColor color;
        QRectF pageRect;
    };

    enum class SourceCommentGeometryType {
        StickyNote,
        Stamp,
        Highlight,
        Underline,
        Squiggly,
        StrikeOut
    };

    struct SourceCommentGeometryUpdate {
        QString markerId;
        int page = -1;
        SourceCommentGeometryType type
            = SourceCommentGeometryType::StickyNote;
        QRectF pageRect;
        QVector<std::array<QPointF, 4>> pageQuadPoints;
    };

    inline static constexpr char kSourceCommentGeometryMarkerKey[]
        = "TrePDFSourceCommentGeometryId";

    struct LinkAnnotation {
        QString markerId;
        int page = -1;
        QRectF pageRect;
        int pageQuarterTurns = 0;
        int destinationPage = -1;
        PDFWebLinks::ManualLink link;
    };

    /**
     * @brief Finalizes marked manual links, their actions, and their visible borders.
     *
     * Locates each /Link annotation by its temporary
     * PDFWebLinks::ManualMarkerKey string (TrePDFLinkEditId), accepting supported
     * indirect objects and direct annotation dictionaries. The caller has already
     * created or marked each target, checked any source fingerprint/permissions,
     * and removed deleted links. This function patches the remaining annotations;
     * it does not perform source-fingerprint checks or link deletion itself.
     *
     * Rebuilds /Rect, /A, /Border, /BS, /C, /H, and TrePDFLinkStyle metadata. A web
     * link receives an encoded HTTP(S) /URI action. An internal link receives a
     * /GoTo action whose /D uses the final page's indirect reference and /Fit,
     * rather than a source-page index. The caller must supply destinationPage
     * after accounting for page insertion/deletion. Obsolete /Dest, /QuadPoints,
     * and the temporary marker are removed.
     *
     * For a source link (link.annotationIndex >= 0), an existing direct or indirect
     * action dictionary is resolved and its unrelated keys retained while the
     * target keys are changed. Actions containing /Next or /IsMap are rejected.
     * A visible border gets a new vector /AP /N, using pageQuarterTurns for its
     * orientation. An invisible border has width zero and its /AP removed.
     * Other annotation dictionary entries are retained by the targeted rewrite.
     *
     * Rejects more than PDFWebLinks::MaximumLinks entries, empty/duplicate marker
     * IDs, invalid ManualLink data, negative source page indices, rotations outside
     * 0..3, and nonfinite/empty or excessively large rectangles. The presence of
     * destinationPage must agree with link.targetSourcePage. Resolving the final
     * page tree is bounded to depth 64 and 200,000 visited nodes; missing pages,
     * unsafe dictionaries, duplicate target objects, and overlapping direct-object
     * rewrites fail the batch before appending.
     *
     * @param filePath Plaintext staging PDF, modified by an incremental append on
     *        success. See the class-level file and failure guarantees.
     * @param annotations Prepared link updates. page and destinationPage refer to
     *        zero-based output-page indices; a negative destinationPage denotes a
     *        web target. pageRect is in PDF user space, and pageQuarterTurns is the
     *        intrinsic page rotation, not the viewer rotation. Marker lookup is
     *        document-wide; the caller is responsible for correct page ownership.
     * @param error Receives a failure diagnostic; is not cleared on success.
     * @return True for an empty list or a successfully appended link update; false
     *         for invalid input, unsupported PDF structure, or an I/O failure.
     */
    static bool finalizeLinkAnnotations(
        const QString &filePath,
        const QVector<LinkAnnotation> &annotations,
        QString &error);

    /**
     * @brief Converts prepared placeholders and supported widgets into canonical fields.
     *
     * Resolves targets by widgetId stored in /NM. Add expects a /Square placeholder
     * already created on the intended page; a direct placeholder is promoted to an
     * indirect /Widget and its containing dictionary is replaced by a reference.
     * Other operations expect an indirect /Widget reachable from the AcroForm field
     * tree. Two requests must not resolve to the same widget. This method does not
     * create placement placeholders or discover arbitrary editable source fields.
     *
     * Operation behavior:
     * - Add and Update build the supported canonical widget dictionaries from the
     *   supplied values, flags, geometry, and style-specific data. Update is not a
     *   generic merge that promises to retain every custom widget key.
     * - NormalizeAppearance normally preserves text/choice field dictionaries while
     *   replacing /AP and the appearance version; managed/default font references
     *   and list-box /TI are adjusted when required. Legacy single-select list boxes
     *   are instead migrated to the current multi-select structure. Checkbox
     *   appearance-only normalization is unsupported. Radio buttons follow the
     *   complete-group rebuilding path, even for this operation.
     * - Remove detaches supported TrePDF Text, Dropdown, List Box, or Radio Button
     *   widgets from the field/page structures. Checkbox removal is unsupported.
     *   Ordinary removals require a top-level single-widget field; old object bytes
     *   remain in the file because this is an incremental update.
     *
     * Radio members sharing fieldName become one parent field with /Kids and unique
     * widget export names. A touched existing radio group must be supplied in full,
     * including removed members and surviving siblings. Members must agree on flags
     * and tooltip, with at most one current selection and one default selection.
     * The supported legacy merged radio-widget form is handled by the same rebuild.
     *
     * Nonremoved fields require a nonempty flat fieldName without '.'. Names must
     * not collide with untouched fields; sharing a name is allowed only within a
     * requested radio group. Choice options must be unique, nonempty, and within
     * implementation bounds. Dropdown values/defaults must be options. List-box
     * selectedValues must be unique and in option order, with value equal to the
     * first selection or empty when none is selected; defaultValue must be an option.
     * Choice flags must describe fixed dropdowns or multi-select list boxes. Text
     * limits and comb/multiline combinations are checked; nontext fields cannot
     * carry a text maximum length or comb flag.
     *
     * Text and choice fields receive raster-backed normal appearances and a shared
     * Unicode form font, reused by font-data fingerprint when possible or embedded
     * otherwise. Their total generated appearance area is capped at 32 * 1024 *
     * 1024 pixels. Checkboxes/radio buttons receive vector off/on appearances.
     * The method updates /AcroForm /Fields and required font resources, creating the
     * AcroForm when absent. It rejects catalog /Perms, direct catalog AcroForm
     * dictionaries, XFA, /SigFlags, signature fields, and unsupported field trees.
     *
     * @param filePath Serialized staging PDF containing the prepared targets.
     * @param fields Complete operation snapshots, not property deltas. Supply valid
     *        PDF-space pageRect values and already normalized field values/flags;
     *        the writer owns placement validation and source-value preparation.
     * @param fontFace Font-byte snapshot and matching layout family for nonremoved
     *        Text/Dropdown/ListBox fields. A newly embedded Unicode resource requires
     *        a supported TrueType face with glyf/loca tables, at most 64 MiB of font
     *        data, a valid PostScript name, and the required U+1ECD glyph. Raster
     *        appearances use layoutFamily when present, otherwise family; that
     *        matching layout font must be available to Qt. Button-only or
     *        removal-only batches do not require this Unicode font snapshot.
     * @param error Receives the diagnostic on failure; is not reset on success.
     * @return True for no operations or successful incremental finalization; false
     *         if validation, resource/appearance generation, or file I/O fails.
     * @note These checks complement the writer's preflight; this API is not a
     *       general-purpose editor for arbitrary third-party AcroForm hierarchies.
     */
    static bool finalizeFields(const QString &filePath,
                               const QVector<Field> &fields,
                               const ApplicationFonts::FontFaceData &fontFace,
                               QString &error);
    /**
     * @brief Connects prepared reply or review-state annotations to their parents.
     *
     * Each request supplies exactly one parent selector: parentThreadId matches a
     * /TrePDFThreadId string, while parentAnnotationId matches /NM. The parent may
     * have any subtype; childAnnotationId must identify a /Text annotation by /NM.
     * Lookup is document-wide, so the caller must establish intended page membership
     * and relationship eligibility before serializing these identifiers.
     *
     * A direct parent dictionary is promoted to an indirect object so /IRT can
     * reference it. Repeated references to that parent reuse the promoted object,
     * and a promoted annotation may also participate as a child in the batch.
     * Supported direct children are patched in their containing objects; indirect
     * children are rewritten without replacing unrelated dictionary entries.
     *
     * Reply adds /IRT and /RT /R. Its child must not have /State or /StateModel,
     * and the request must not supply expectedState or expectedStateModel. State
     * adds /IRT while retaining the child's existing /State and /StateModel; both
     * must be present, decodable as PDF names or strings, and valid bounded names.
     * Nonempty expectedState/expectedStateModel values additionally require exact
     * matches with those stored values. This method links an already prepared state
     * annotation; it does not create state values, authors, timestamps, or contents.
     *
     * Both kinds reject a child that already has /IRT or /RT. Child IDs must be
     * nonempty and unique, parent selectors must be unambiguous in the request, and
     * direct self-reference or reuse of one resolved child is rejected. State-name
     * constraints allow only 1..64 ASCII letters, digits, underscores, or hyphens.
     * These checks do not constitute a full cycle analysis of the resulting thread
     * graph, nor do they establish global uniqueness of IDs in arbitrary PDFs.
     *
     * @param filePath Staging PDF containing the parent and child annotations.
     * @param relationships Prepared links. page must be nonnegative, but is not used
     *        to scope lookup or prove that parent and child belong to that page.
     * @param error Receives a failure diagnostic; success leaves it unchanged.
     * @return True for an empty list or a successfully appended set of links; false
     *         for invalid relationships, missing/malformed targets, unsafe or
     *         overlapping rewrites, exhausted object numbers, or parser/I/O failures.
     * @note Does not generate appearances. Repeating a completed nonempty request
     *       normally fails because its children now have /IRT; this is not an
     *       idempotent relationship-repair operation.
     */
    static bool finalizeAnnotationRelationships(
        const QString &filePath,
        const QVector<AnnotationRelationship> &relationships,
        QString &error);
    /**
     * @brief Builds readable normal appearances for marked Free Text annotations.
     *
     * Finds /FreeText targets through the temporary /TrePDFFreeTextAppearanceId
     * string, using annotationId as its value. This lookup does not use /NM.
     * Reads each existing /Rect as a finite four-number array with nonzero absolute
     * width and height; those dimensions determine the local appearance frame.
     * The caller must already have written semantic /Contents. The supplied text
     * drives rasterization only and is not written back to /Contents here.
     *
     * Generates an RGB image and a Form XObject that paints it as /AP /N. Rendering
     * uses a white background, a black one-point border, four-point padding, dark
     * text, the shared text-field font size, and top-left word-boundary-or-anywhere
     * wrapping clipped to the frame. The target raster scale is two pixels per PDF
     * point, with each dimension clamped to 2..2048 pixels. Existing rich-text/font
     * instructions in the annotation are not interpreted to reproduce their style.
     *
     * Replaces the normal appearance dictionary and /BS, writes the TrePDF Free
     * Text appearance-version marker, and removes the temporary lookup marker.
     * /DA is replaced unless preserveDefaultAppearance is true and a /DA already
     * exists; a missing /DA is filled even when preservation was requested. That
     * flag preserves dictionary data, not the visual styling of the generated image.
     *
     * If any request allows /DA replacement and the catalog AcroForm is not direct,
     * the method reuses a matching Unicode form-font resource by font-byte hash or
     * embeds one, updating/creating AcroForm font resources while retaining existing
     * fields. With a direct AcroForm, that installation is skipped and generated
     * default-appearance strings use the Helv resource name. Preserved appearances
     * do not by themselves require embedding a new Unicode form font.
     *
     * @param filePath Staging PDF with the prepared, marked Free Text annotations.
     * @param annotations Nonnegative page values and nonempty unique annotation IDs
     *        are required. page is not checked against page count or used for lookup.
     *        Each text is limited here to 1,000,000 UTF-16 code units; UI text limits
     *        are enforced elsewhere. Request text and serialized /Contents must
     *        describe the same content to keep appearance and semantics consistent.
     * @param fontFace Font snapshot for Unicode resources when needed, and a Qt
     *        layout family for rasterization (layoutFamily, falling back to family).
     *        The caller must supply an available matching layout font; when a new
     *        Unicode resource is built, supported TrueType bytes are also required.
     * @param error Receives a diagnostic on failure; is not cleared on success.
     * @return True for an empty list or completed incremental finalization; false
     *         for missing/invalid targets, invalid rectangles, duplicate/overlapping
     *         rewrites, font/image generation problems, or parser/write failures.
     */
    static bool finalizeFreeTextAnnotations(
        const QString &filePath,
        const QVector<FreeTextAnnotation> &annotations,
        const ApplicationFonts::FontFaceData &fontFace,
        QString &error);
    /**
     * @brief Replaces text-markup appearances with subtype-specific vector artwork.
     *
     * Matches annotationId against /NM and requires the annotation's subtype to
     * agree with Highlight, Underline, Squiggly, or StrikeOut. The caller must have
     * already serialized /QuadPoints and color/opacity. This pass reads those
     * properties and changes /AP; it does not update /Contents, /Rect, /QuadPoints,
     * /C, or /CA, and it does not search page text to infer the marked glyphs.
     *
     * /QuadPoints must be a direct, nonempty numeric array containing complete
     * groups of eight finite numbers, with at most 4096 quadrilaterals. Each group
     * is reduced to its axis-aligned bounding rectangle and must have positive
     * extent on both axes. The generated appearance therefore does not reproduce
     * arbitrary rotated/skewed quad outlines; the writer is responsible for supplying
     * geometry compatible with this axis-aligned rendering contract.
     *
     * Requires a three-component RGB /C with components in [0, 1]. Optional /CA
     * defaults to 1 and, when present, must also be finite and within [0, 1]. The
     * Form appearance covers the union of the quad bounds and applies the opacity
     * through an ExtGState. Highlights fill each rectangle with Multiply blending.
     * Underline, strikeout, and squiggly use normal blending and one-point strokes;
     * their paths follow the bottom inset, vertical midpoint, or zigzag respectively.
     *
     * A new Form XObject replaces the whole /AP dictionary with a single normal
     * appearance reference, so old rollover/down entries are not retained. Artwork
     * generation has an approximately 8 MiB per-annotation content bound. No font
     * embedding or page-content regeneration is performed.
     *
     * @param filePath Staging PDF containing the prepared markup annotations.
     * @param annotations Unique nonempty IDs with a supported type and page >= 0.
     *        Lookup is document-wide by ID/subtype; page membership is a caller
     *        precondition rather than something established by this method.
     * @param error Receives the failure diagnostic; is not reset on success.
     * @return True for an empty list or successfully appended appearances; false
     *         for invalid requests, missing targets, malformed geometry/color,
     *         unsafe rewrites, resource limits, or parser/file-I/O errors.
     */
    static bool finalizeTextMarkupAnnotations(
        const QString &filePath,
        const QVector<TextMarkupAnnotation> &annotations,
        QString &error);
    /**
     * @brief Finalizes prepared drawing comments and creates canonical vector appearances.
     *
     * Supports Square, Circle, Line, Polygon, PolyLine, and Ink annotations already
     * present in the staging PDF. Each target is located by annotationId in /NM
     * together with its expected subtype; this function does not create annotation
     * placeholders or validate source-document editing permission/fingerprints.
     * Supported direct dictionaries and indirect annotation objects can be patched.
     *
     * Writes canonical /Rect, /Subtype, /C, /CA, /F, and /BS values. Depending on
     * type, it writes /L, /Vertices, or /InkList and the applicable line-ending,
     * interior-color, intent, and name entries. An existing /Border is neutralized.
     * Optional entries are written according to the supplied supported properties;
     * this is not a general removal of every stale optional key. In particular,
     * absence of a new interior color does not itself erase an existing /IC here.
     * A new Form XObject replaces the whole /AP dictionary with one /N reference.
     * Unrelated annotation dictionary entries are retained by the targeted rewrite.
     *
     * Canonical artwork requires finite, bounded PDF-space geometry and a positive
     * rectangle. Square/Circle/Line require exactly two vertices; Polygon requires
     * at least three and PolyLine at least two, with at most 4096 vertices. Adjacent
     * duplicate vertices and an explicitly repeated polygon closing point are
     * rejected. Ink instead uses 1..64 strokes, 2..4096 points per stroke and at most
     * 32768 points per annotation, with no ordinary vertices or interior fill.
     *
     * Appearance generation accepts the implemented solid/dashed border styles,
     * valid stroke colors, opacity in [0, 1], and positive stroke widths up to 1024
     * PDF points. Unsupported border effects or non-None line endings are rejected
     * rather than silently approximated. Coordinate magnitudes are bounded to
     * 10,000,000 PDF points. Limits also include 4096 requests, 65536 total geometry
     * points, approximately 8 MiB of artwork per annotation, and 64 MiB per batch.
     *
     * @param filePath Staging PDF containing the expected /NM-marked placeholders.
     * @param annotations Complete PDFCommentAnnotation drawing snapshots with
     *        nonempty unique annotationId, supported subtype, and page >= 0.
     *        Rectangles/vertices/strokes must already be in PDF page coordinates.
     *        The page field is not used to scope marker lookup or prove ownership.
     * @param error Receives validation, appearance, parser, or I/O diagnostics;
     *        successful and empty calls do not clear it.
     * @return True for no requests or a completed incremental batch; false if any
     *         request cannot be represented safely, resolved, or written.
     */
    static bool finalizeDrawingAnnotations(
        const QString &filePath,
        const QVector<PDFCommentAnnotation::DrawingAnnotation> &annotations,
        QString &error);
    /**
     * @brief Builds the canonical vector appearance for each prepared preset stamp.
     *
     * Resolves a /Stamp annotation by annotationId in /NM, then obtains shared
     * artwork from PDFStampPresets using stampName, displayText, and detailText.
     * The preset's 160-by-50 top-left artwork coordinates are transformed into the
     * supplied PDF-space rectangle. Text is represented by vector outlines,
     * including supported Unicode detail/author text, so the PDF appearance does
     * not depend on installing separate PDF font resources.
     *
     * Rewrites /Subtype, normalized /Rect, /Name, /C, /CA, and /Border [0 0 0], and
     * replaces /AP with a new normal Form appearance. Its background uses the
     * supplied stamp color and the shared preset supplies the foreground artwork.
     * Existing /Contents, /NM, and unrelated dictionary entries remain intact.
     * The caller owns semantic contents and any author/date snapshot; this method
     * does not recompute dynamic stamp information or inspect the current clock.
     *
     * stampName must be a known preset and a safe PDF name of 1..64 ASCII letters,
     * digits, underscores, or hyphens. displayText must contain 1..32 UTF-16 code
     * units of uppercase ASCII letters/spaces. detailText is limited to 256 valid
     * UTF-16 code units without control characters or line/paragraph separators.
     * Color, finite positive rectangle geometry, and generated artwork elements
     * are validated. Coordinate magnitudes are bounded to 10,000,000 PDF points.
     * The batch permits at most 4096 requests and 64 MiB of generated artwork.
     *
     * @param filePath Staging PDF with existing stamp placeholders to finalize.
     * @param annotations Complete preset/appearance snapshots, with nonempty unique
     *        annotation IDs and page >= 0. pageRect is already in PDF user space.
     *        Lookup is by ID/subtype across the document, not scoped by page.
     * @param error Receives a diagnostic on failure; is left unchanged on success.
     * @return True for an empty list or successfully appended appearances; false
     *         for invalid presets/data, unresolved targets, unsafe direct/indirect
     *         rewrites, generation limits, or parser/file-I/O failures.
     */
    static bool finalizeStampAnnotations(
        const QString &filePath,
        const QVector<StampAnnotation> &annotations,
        QString &error);
    /**
     * @brief Patches marked source drawing geometry and supported appearance properties.
     *
     * Finds each existing drawing by its temporary TrePDFSourceDrawingGeometryId
     * marker and expected subtype. The caller must first establish that the source
     * annotation is editable and, for appearance regeneration, that its provenance
     * and properties are supported. This pass does not revalidate a source
     * fingerprint or infer those permissions from arbitrary appearance content.
     *
     * When geometryChanged is true, replaces the normalized /Rect and the subtype's
     * /L, /Vertices, or /InkList data. When propertiesChanged is true, patches /C,
     * /CA, and applicable /IC; /IC is removed for Ink or an absent valid interior
     * color. Other property entries, including /BS, /BE, and /LE, are retained.
     * The temporary marker is removed after a successful dictionary rewrite.
     *
     * Existing /AP is preserved unless regenerateAppearance requests a replacement
     * that can be produced and installed safely. Regeneration patches only /AP /N,
     * preserving other appearance entries. Supported replacement structures are an
     * absent /AP or a direct /AP dictionary whose /N is absent or an indirect object
     * reference; an indirect /AP dictionary or a custom normal-state dictionary
     * cannot be replaced through this path. Original appearance streams and their
     * /BBox are never edited in place.
     *
     * For geometry-only updates, regeneration is best effort: an unsupported
     * appearance structure, generation failure, or exhausted appearance budget may
     * leave the existing /AP intact while the geometry update succeeds. Property
     * updates require regenerateAppearance=true and a successful replacement;
     * otherwise the batch fails to avoid storing a style that is not visible.
     *
     * Requests must include at least one changed aspect, a nonempty unique marker,
     * and a complete valid drawing snapshot even for property-only changes. Source
     * geometry accepts the supported subtype-specific vertex/stroke structures,
     * finite coordinates within 10,000,000 PDF points, positive rectangles, and no
     * adjacent duplicate points or repeated polygon closing point. Limits include
     * 4096 updates, 65536 total geometry points, and 64 MiB of regenerated content.
     * Canonical regeneration adds the stricter drawing-appearance constraints used
     * by finalizeDrawingAnnotations().
     *
     * @param filePath Serialized staging PDF containing the temporary source markers.
     * @param updates Complete source drawing updates. Geometry is already in PDF
     *        coordinates; annotation.page must be nonnegative but does not scope
     *        lookup or validate ownership. Geometry and property flags determine
     *        which dictionary fields this pass changes.
     * @param error Receives a failure diagnostic; is not cleared on success,
     *        including geometry-only success that retains the original appearance.
     * @return True for an empty list or successful incremental patching, including
     *         geometry-only success that retains /AP. False for invalid updates,
     *         missing targets, required regeneration failure, or parser/write errors.
     */
    static bool finalizeSourceDrawingGeometryUpdates(
        const QString &filePath,
        const QVector<PDFCommentAnnotation::SourceDrawingGeometryUpdate> &updates,
        QString &error);
    /**
     * @brief Updates source comment rectangles/quads while retaining their appearances.
     *
     * Resolves each annotation using kSourceCommentGeometryMarkerKey and the
     * subtype selected by SourceCommentGeometryType: StickyNote maps to /Text,
     * Stamp to /Stamp, and markup types to their corresponding PDF subtype names.
     * Supported direct dictionaries and indirect annotation objects are patched
     * without asking PDFium to resize their appearance streams.
     *
     * Always writes a normalized /Rect. Highlight, Underline, Squiggly, and StrikeOut
     * also receive replacement /QuadPoints in the supplied order. Sticky Note and
     * Stamp requests must not carry quad points. Removes the temporary marker and
     * preserves /AP, appearance stream /BBox values, and unrelated dictionary keys.
     * No new appearance is generated, so custom source artwork remains in use.
     *
     * Rectangles must be finite and have positive width/height; coordinate magnitudes
     * are bounded to 10,000,000 PDF points. Markup requests need 1..4096 quads each,
     * with finite bounded points and positive x/y extent for every quad's bounding
     * box. This check does not prove convexity, point ordering, or absence of
     * self-intersection: the caller must provide valid PDF quad geometry. At most
     * 4096 updates and 65536 individual quad points are accepted per batch.
     *
     * @param filePath Staging PDF in which the caller has already marked eligible
     *        source comments after checking their identity and editability.
     * @param updates Nonempty unique marker IDs, supported types, page >= 0, and
     *        rectangles/quads already converted to PDF page coordinates. The page
     *        value does not limit marker lookup or establish page membership.
     * @param error Receives a failure diagnostic; is not cleared on success.
     * @return True for an empty list or successfully appended geometry changes;
     *         false for invalid geometry, missing/mismatched targets, duplicate or
     *         overlapping rewrites, unsupported structure, or file-I/O failure.
     * @note This preserves source /AP data; it does not promise that arbitrary
     *       custom artwork will redraw itself to follow newly supplied quad points.
     */
    static bool finalizeSourceCommentGeometryUpdates(
        const QString &filePath,
        const QVector<SourceCommentGeometryUpdate> &updates,
        QString &error);
};
