#pragma once

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <fpdf_annot.h>

/**
 * @brief Shared readers and classifiers for PDF annotation metadata and comments.
 *
 * Helpers accepting an FPDF_ANNOTATION borrow that handle: they neither take
 * ownership nor close it. The caller must keep the annotation and its owning
 * PDFium document/page valid throughout the call, and must already hold TrePDF's
 * process-wide PDFium lock. These functions assert the lock requirement before
 * checking their arguments, including when the supplied annotation is null.
 * They do not acquire the lock or schedule work on a worker thread themselves.
 *
 * String-only and integer-subtype helpers do not access PDFium and do not require
 * that lock. All functions return independent values rather than retained PDFium
 * handles; normalAppearanceFingerprint() borrows its QStringView only during the
 * call. None of these helpers modifies annotation dictionaries or saves a PDF.
 *
 * Classification is deliberately separate from editing eligibility. A recognized
 * comment or markup subtype may still be read-only, locked, structurally unsafe,
 * or unsupported for a particular operation. Callers must apply those additional
 * checks before offering editing, deletion, or relationship changes.
 */
namespace PDFAnnotationUtils {

/**
 * @brief Reads a bounded PDF string or name from an annotation dictionary.
 *
 * Uses FPDFAnnot_GetStringValue() twice: first to obtain the required byte count,
 * then to fill a buffer of FPDF_WCHAR units. The reported allocation size must be
 * at least one UTF-16 code unit, no greater than 4 MiB (4 * 1024 * 1024 bytes),
 * and divisible by sizeof(FPDF_WCHAR). The second read must return a positive
 * aligned size that fits the allocation; a shrinking value is accepted, but an
 * oversized or otherwise inconsistent result is rejected without retrying.
 *
 * Removes every trailing U+0000 code unit, including the API's string terminator,
 * then constructs a QString from the remaining UTF-16 data. Embedded nulls are
 * retained. No whitespace trimming, newline conversion, case conversion, PDF name
 * interpretation beyond PDFium's decoding, or additional surrogate validation is
 * performed here. Call normalizedCommentText() separately when reading comment
 * contents that need editor-style LF line endings.
 *
 * PDFium accepts both string and name objects through this getter. Missing keys,
 * unsupported value types, and an actual empty value all produce the same empty
 * result here; a null argument, a rejected size, or a PDFium read error also does.
 * Consequently, this function is not a dictionary-key existence/type test.
 *
 * @param annotation Borrowed annotation handle. A null handle returns an empty string.
 * @param key Null-terminated UTF-8 dictionary key without the leading '/', such as
 *        "Contents", "NM", "Subtype", or "State". A null pointer returns empty.
 * @return Decoded value, or an empty QString when no acceptable value can be read.
 *         The return value does not distinguish absence, emptiness, and failure.
 * @pre The caller holds the process-wide PDFium lock, even for null arguments.
 */
QString annotationString(FPDF_ANNOTATION annotation, FPDF_BYTESTRING key);

/**
 * @brief Reads the normal appearance string exposed by PDFium, with a size bound.
 *
 * Calls FPDFAnnot_GetAP() for FPDF_ANNOT_APPEARANCEMODE_NORMAL. A size query precedes
 * the actual read, and the first reported size must be aligned to FPDF_WCHAR,
 * at least one code unit, and at most 1 MiB (1024 * 1024 bytes). The second read
 * must return an aligned size within the allocated buffer. Invalid lengths or
 * failed reads return empty rather than allocating unbounded appearance data.
 *
 * Strips all trailing U+0000 units and converts the remaining UTF-16 data to a
 * QString, retaining embedded nulls and all other content as decoded by PDFium.
 * This is the appearance string returned by that API, not the original compressed
 * stream bytes, a rendered image, the complete /AP dictionary, or the annotation's
 * /Contents. Rollover and down appearances are not requested. The helper does not
 * interpret drawing operators or collect referenced resources, /BBox, or /Matrix.
 *
 * A missing normal appearance, an empty appearance, an oversized response, and
 * an API failure are intentionally indistinguishable through the empty result.
 * Use a separate structural check when that distinction matters.
 *
 * @param annotation Borrowed annotation handle; null returns an empty QString.
 * @return Bounded decoded normal-appearance text, or empty when unavailable/rejected.
 * @pre The caller holds the process-wide PDFium lock for the entire call.
 * @see normalAppearanceFingerprint()
 */
QString annotationNormalAppearance(FPDF_ANNOTATION annotation);

/**
 * @brief Produces a SHA-256 identity token for nonempty appearance text.
 *
 * Hashes exactly appearance.size() UTF-16 code units using their in-memory bytes:
 * length * sizeof(char16_t), including embedded nulls and any trailing nulls that
 * are explicitly part of the view. No null terminator is added. The function does
 * not encode UTF-8, normalize Unicode/newlines/whitespace, or parse PDF operators.
 * Two visually equivalent appearances with different text can therefore have
 * different fingerprints.
 *
 * The usual input is annotationNormalAppearance(), which has already removed its
 * trailing null units. This helper itself imposes no appearance-size cap and does
 * not inspect the owning PDF, annotation properties, or referenced resources.
 * It is an identity/change-detection token for the supplied text, not proof of
 * complete visual equivalence, document integrity, or authorization to edit.
 *
 * Because raw UTF-16 storage is hashed, byte order follows the host representation;
 * this is not an explicitly endian-independent interchange hash. Empty input uses
 * an empty QByteArray sentinel instead of the standard SHA-256 digest of zero bytes.
 *
 * @param appearance Borrowed UTF-16 view, which must remain valid until the call ends.
 * @return Raw 32-byte SHA-256 digest for nonempty input, or an empty QByteArray.
 *         The digest is binary; call toHex() separately if a textual form is needed.
 * @note Does not access PDFium and does not require its process-wide lock.
 */
QByteArray normalAppearanceFingerprint(QStringView appearance);

/**
 * @brief Converts comment line endings to the LF convention used by the editor.
 *
 * Replaces each CRLF pair with one LF first, then replaces remaining standalone
 * CR characters with LF. This ordering avoids turning a Windows line ending into
 * two line breaks. Existing LF characters, blank lines, leading/trailing spaces,
 * embedded nulls, and other Unicode characters are retained. Unicode line/paragraph
 * separators are not converted, and no Unicode normalization or length limit is
 * applied. Reapplying the function to its output leaves that output unchanged.
 *
 * @param text Comment contents passed by value; the caller's QString is not modified.
 * @return The transformed string, including an empty string when input is empty.
 * @note This only prepares in-memory/editor text. It does not write /Contents or
 *       convert line endings back to the PDF serialization convention, and requires
 *       no PDFium lock.
 */
QString normalizedCommentText(QString text);

/**
 * @brief Tests membership in TrePDF's implemented comment-subtype set.
 *
 * Returns true for Text (sticky notes), FreeText, Highlight, Underline, Squiggly,
 * StrikeOut, Square, Circle, Line, Polygon, PolyLine, Ink, and Stamp. This fixed
 * list is narrower than the PDF markup category exposed by
 * isMarkupAnnotationSubtype(); for example, Caret, FileAttachment, Sound, and
 * Redact are markup types but are not included in this supported-comment list.
 *
 * This checks only an integer subtype. It does not inspect annotation flags,
 * geometry, appearance provenance, reply/state metadata, or supported operations.
 * In particular, true is not a blanket guarantee that a particular source
 * annotation can be edited, resized, restyled, or deleted safely.
 *
 * @param subtype PDFium FPDF_ANNOT_* subtype value, normally from FPDFAnnot_GetSubtype().
 * @return True for one of the listed supported subtypes; false for all other values,
 *         including unknown values, Link, Widget, and Popup.
 * @note This helper does not access PDFium. Obtaining subtype from a live handle
 *       remains the caller's responsibility and requires the usual PDFium lock.
 */
bool isSupportedCommentAnnotationSubtype(int subtype);

/**
 * @brief Tests whether a PDFium subtype belongs to the recognized PDF markup category.
 *
 * Includes Text, FreeText, Line, Square, Circle, Polygon, PolyLine, Highlight,
 * Underline, Squiggly, StrikeOut, Stamp, Caret, Ink, FileAttachment, Sound, and
 * Redact. Here "markup" means the broad PDF annotation category used in comment
 * relationships; it is not limited to the four text-highlighting/decoration tools.
 * The list intentionally includes types that TrePDF may expose only as read-only.
 *
 * This integer-only predicate cannot recognize a Projection annotation when
 * PDFium reports it as FPDF_ANNOT_UNKNOWN. Use isMarkupAnnotation() when a live
 * handle is available and that dictionary-level extension must be considered.
 * No editing-eligibility, flags, appearance, or parent-relationship checks occur.
 *
 * @param subtype PDFium annotation subtype code; no handle is retained or inspected.
 * @return True for a listed markup type, false for every other integer value.
 * @note Does not access PDFium and does not require its process-wide lock.
 */
bool isMarkupAnnotationSubtype(int subtype);

/**
 * @brief Recognizes the Projection subtype not represented by a dedicated PDFium enum.
 *
 * Requires both FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_UNKNOWN and an
 * annotationString(annotation, "Subtype") result exactly equal to "Projection".
 * The comparison is case-sensitive and does not trim whitespace. If PDFium reports
 * a known enum subtype, this compatibility check returns false without accepting
 * a conflicting dictionary string. The bounded string reader's empty/error result
 * also prevents a match.
 *
 * This only identifies the subtype; it does not inspect projection-specific data,
 * validate an appearance, or make the annotation editable. It allows callers such
 * as isMarkupAnnotation() to retain the broader annotation classification despite
 * the unknown PDFium enum value.
 *
 * @param annotation Borrowed annotation handle; null returns false.
 * @return True only for the unknown-enum plus exact "Projection" combination.
 * @pre The caller holds the process-wide PDFium lock.
 */
bool isProjectionAnnotation(FPDF_ANNOTATION annotation);

/**
 * @brief Classifies a live annotation as markup, including the Projection extension.
 *
 * Accepts any subtype recognized by isMarkupAnnotationSubtype(), or a Projection
 * annotation recognized by isProjectionAnnotation(). The latter dictionary check
 * is only needed when the integer subtype is not already in the markup list.
 * A null annotation is rejected.
 *
 * This broad classification is useful for comment/thread handling, including
 * markup types without editing support. It does not prove that /IRT is present,
 * that a parent is valid, or that a comment is an ordinary visible reply rather
 * than a review-state record. Use the relationship/state helpers and the caller's
 * source-safety checks for those separate decisions.
 *
 * @param annotation Borrowed live annotation; it is never closed by this helper.
 * @return True for a recognized markup subtype or Projection, otherwise false.
 * @pre The caller holds the process-wide PDFium lock throughout the call.
 * @see isSupportedCommentAnnotationSubtype()
 * @see isCommentStateAnnotation()
 */
bool isMarkupAnnotation(FPDF_ANNOTATION annotation);

/**
 * @brief Identifies a linked Text annotation used as a review/checkmark state record.
 *
 * Requires a non-null /Text annotation and a reply-compatible /RT according to
 * annotationUsesReplyRelation(). Reads and trims /State and /StateModel through
 * the bounded annotationString() helper. /State must be nonempty, and /StateModel
 * must compare case-insensitively with "Review" or "Marked". Finally, /IRT must
 * resolve through FPDFAnnot_GetLinkedAnnot() to a non-null annotation handle.
 * That temporary parent handle is closed before this function returns; the input
 * annotation remains borrowed and is not closed.
 *
 * This is a structural recognition predicate, not a full state-transition
 * validator. It does not restrict /State to a known enumerated value, inspect the
 * parent's subtype or page, verify authors/timestamps, check for cycles, or decide
 * which transition is the current state. It also does not mutate or hide anything
 * itself; callers use the result to keep state records distinct from visible
 * comment/reply contents while retaining their relationships.
 *
 * @param annotation Borrowed candidate annotation; null or a non-Text subtype
 *        returns false. Missing/unreadable required state data or an unresolved
 *        parent also returns false.
 * @return True when the subtype, relation convention, state/model strings, and
 *         resolvable /IRT parent satisfy the checks above.
 * @pre The caller holds the process-wide PDFium lock, including while the temporary
 *      linked-parent handle is opened and closed.
 */
bool isCommentStateAnnotation(FPDF_ANNOTATION annotation);

/**
 * @brief Tests whether /RT uses the default or explicit reply-relation convention.
 *
 * Reads /RT through annotationString(), trims surrounding whitespace, and accepts
 * either an empty value or "R" in a case-insensitive comparison. A missing /RT
 * therefore uses the default reply convention. Other nonempty values, such as
 * "G" for a group relation, are rejected.
 *
 * This does not check /IRT, resolve a parent, or constrain the annotation subtype.
 * A true result alone does not mean that the annotation is actually a reply.
 * Because the shared reader represents missing, empty, unsupported, oversized,
 * and unreadable values with the same empty string, all of those outcomes pass
 * this predicate. In particular, a null annotation also returns true after the
 * lock assertion. Callers that require a real relationship must separately require
 * a valid annotation and a resolvable /IRT parent, as isCommentStateAnnotation() does.
 *
 * @param annotation Borrowed annotation whose relation convention is being tested.
 * @return True when the trimmed /RT value is empty or equals "R" ignoring case;
 *         false for any other nonempty value.
 * @pre The caller holds the process-wide PDFium lock, even when annotation is null.
 */
bool annotationUsesReplyRelation(FPDF_ANNOTATION annotation);

} // namespace PDFAnnotationUtils
