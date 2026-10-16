#pragma once
#include "IFontProvider.h"

/**
 * @brief Describes the Google Fonts lookup and download rules used by TrePDF.
 *
 * This provider is a stateless adapter between FontDownloadService and three
 * endpoints with different responsibilities:
 * - Fontsource metadata identifies a Google Fonts family, its available weights,
 *   styles, and language subsets.
 * - The Google Fonts CSS endpoint describes the requested weight/style.
 * - The Google static-font endpoint supplies the referenced TrueType file.
 *
 * The normal sequence is metadataUrl() -> resolve() -> faceUrl() -> fontUrl().
 * These methods only construct URLs or inspect already downloaded bytes. They do
 * not issue requests, retain response data, install fonts, or access PDF contents.
 * FontDownloadService owns consent, transport, timeouts, response-size limits,
 * cancellation, font-byte/glyph validation, and installation of the completed batch.
 *
 * Exact face availability is represented by the Regular/Italic/Bold/Bold Italic
 * flags in IFontProvider::Family. A missing style is not synthesized or replaced
 * with another weight. CSS is accepted conservatively to avoid installing a
 * language subset as though it were a complete editable face; actual font coverage
 * still has to be checked after downloading the selected file.
 *
 * This class inherits the interface's empty fallbackFaceUrls() implementation, so
 * it supplies no alternate download mirrors. It also inherits allowsRedirect():
 * a redirect target must pass allowsUrl() for the same resource category. Redirect
 * counts, HTTP status checks, and original Location path checks belong to the
 * download service rather than this provider.
 */
class GoogleFontsProvider final : public IFontProvider
{
public:
    /**
     * @brief Builds the Fontsource catalogue query for a requested family name.
     *
     * Uses https://api.fontsource.org/v1/fonts with a single "family" query item,
     * assembled through QUrlQuery. The argument is used as supplied: this method
     * does not trim it, remove PDF subset prefixes, check its length, or determine
     * whether the family exists. FontDownloadService performs request-name
     * normalization and validation before invoking the provider.
     *
     * Unlike a provider backed only by a local catalogue, this implementation
     * always returns a metadata endpoint. Constructing the URL performs no network
     * I/O and does not imply that the server will return a matching family.
     *
     * @param family Family name to place in the catalogue query. Pass the normalized
     *        name prepared by the caller, not document text or a document path.
     * @return Metadata request URL, subsequently checked and fetched by the service.
     */
    QUrl metadataUrl(const QString &family) const override;

    /**
     * @brief Resolves an exact Google Fonts family from a Fontsource JSON response.
     *
     * Parses metadata as a JSON array containing at most 32 entries. The array is
     * visited in order; encountering a non-object before resolution ends produces
     * Invalid. Object records whose "family" does not match the request
     * case-insensitively, or whose "type" is not exactly "google", are skipped.
     * Unrelated object records do not undergo the matching-family field checks.
     *
     * The first matching Google record must contain array-valued "weights",
     * "styles", and "subsets" fields. Its subsets array may contain at most 32
     * items; every item must match the pattern ^[a-z][a-z0-9-]{0,39}$. Subsets are
     * retained in their original order and joined with commas for the CSS query.
     * They are not deduplicated. An exact "vietnamese" entry sets the coverage flag
     * that instructs the service to check Vietnamese glyphs in downloaded faces.
     *
     * Face flags are derived from the presence of numeric weights 400/700 and
     * exact style strings "normal"/"italic", in this fixed order:
     * - styles[0]: weight 400 plus normal (Regular).
     * - styles[1]: weight 400 plus italic (Italic).
     * - styles[2]: weight 700 plus normal (Bold).
     * - styles[3]: weight 700 plus italic (Bold Italic).
     * Other weights/styles do not create faces, and string weights such as "400"
     * do not satisfy the numeric checks. A valid matching record may still produce
     * Found with all four flags false; the service checks the requested style.
     *
     * On success, the result keeps the metadata's family spelling, sets source to
     * https://fonts.google.com/, and assigns the fixed provenance label
     * "Google Fonts open-source catalogue" as license. That label is not a parsed
     * per-font license document. This method does not inspect font bytes, prove
     * character coverage, or perform another network lookup.
     *
     * Resolution returns immediately on the first matching record's success or
     * failure; later records are not inspected. A malformed matching record is not
     * bypassed in search of a later duplicate. Callers must inspect availability
     * before using any other result fields, which may be partially populated on
     * failure.
     *
     * @param family Normalized family name to match, without further normalization
     *        by this method.
     * @param metadata Complete response bytes. The service applies the byte-size
     *        limit; this parser additionally bounds record and subset counts.
     * @return Family with Found for an accepted matching Google record, NotFound
     *         when traversal ends without a match (including an empty array), or
     *         Invalid for malformed JSON/non-array input, an oversized array, an
     *         encountered non-object, or invalid fields in the matching record.
     */
    Family resolve(const QString &family, const QByteArray &metadata) const override;

    /**
     * @brief Identifies the resource returned by faceUrl() as a stylesheet.
     *
     * The service must fetch and parse this intermediate resource with fontUrl()
     * before requesting font bytes. Treating faceUrl() as a direct font download
     * would instead feed CSS into font validation. This constant also means the
     * service's retry path for providers of direct font files does not apply here.
     *
     * @return Resource::Stylesheet for every family and style; no state is consulted.
     */
    Resource faceResource() const override { return Resource::Stylesheet; }

    /**
     * @brief Builds the Google Fonts CSS request for one exact weight/style.
     *
     * Uses https://fonts.googleapis.com/css with two QUrlQuery items. The "family"
     * value combines family.name with ":400" or ":700" and appends "italic" when
     * requested; for example, a bold italic face uses "Family Name:700italic".
     * The "subset" item contains family.subsets, including when that string is
     * empty. The subset list comes from resolve(), not from the document's text.
     *
     * This method only constructs the request. It does not recheck availability,
     * the four style flags, or the family fields, and does not choose a fallback
     * weight when the requested face is absent. The caller must first accept a
     * Found result and confirm that its requested style is offered. The resulting
     * CSS must subsequently pass fontUrl() before a font file can be downloaded.
     *
     * @param family Accepted metadata result supplying the canonical name and the
     *        comma-separated subset list.
     * @param bold Selects weight 700 when true, otherwise weight 400.
     * @param italic Adds the italic style suffix when true.
     * @return Stylesheet request URL; no download or font registration occurs here.
     */
    QUrl faceUrl(const Family &family, bool bold, bool italic) const override;

    /**
     * @brief Extracts a single permitted TrueType URL from a stylesheet response.
     *
     * Decodes the bytes as UTF-8 and searches for lowercase url(...) expressions
     * containing a lowercase https:// URL. The regular expression allows whitespace
     * inside the parentheses and optional single or double quotes. There must be
     * exactly one match recognized by that expression. Its parsed URL must also
     * pass allowsUrl(..., Resource::Font), including the host/path and no-query rules.
     *
     * Rejects the entire response if "unicode-range" occurs anywhere in its text,
     * case-insensitively, or if a second recognized URL exists. These conservative
     * checks avoid selecting one web-language subset or arbitrarily choosing among
     * multiple resources. Repeated occurrences of the same recognized URL are also
     * rejected. The substring check includes text inside CSS comments.
     *
     * This is a deliberately narrow extractor, not a general CSS parser: it does
     * not interpret @font-face blocks, escaped URLs, local() alternatives, format()
     * declarations, or family/style declarations. Unsupported URL syntaxes are not
     * counted as matches. Acceptance therefore identifies a permitted candidate
     * resource, not proof that its font bytes are valid or contain every glyph.
     * FontDownloadService performs those checks after fetching the file.
     *
     * @param stylesheet Complete CSS response, bounded in size by the service.
     * @return The single accepted font URL, or an empty QUrl for no recognized URL,
     *         multiple matches, a unicode-range marker, or a disallowed font URL.
     */
    QUrl fontUrl(const QByteArray &stylesheet) const override;

    /**
     * @brief Checks a parsed URL against the allowlist for its resource category.
     *
     * First applies IFontProvider::isSecureUrl(): the URL must be valid HTTPS with
     * a nonempty host, no user information, no explicit port (even 443), and no
     * fragment. The corresponding endpoint constraints are then checked:
     * - Metadata: host api.fontsource.org and exact path /v1/fonts.
     * - Stylesheet: host fonts.googleapis.com and exact path /css.
     * - Font: host fonts.gstatic.com, path beginning with /s/ and ending with .ttf,
     *   and no query component.
     *
     * Metadata and stylesheet queries are permitted without inspecting their keys
     * or values here; metadataUrl()/faceUrl() are responsible for constructing the
     * intended queries. Font queries are rejected even when otherwise on the
     * approved host. Path comparisons use the case-sensitive QString checks in the
     * implementation, so, for example, a .TTF suffix does not satisfy the font rule.
     *
     * The inherited allowsRedirect() uses this same destination check for a given
     * resource type; it does not introduce a separate Google redirect-host list.
     * This method does not perform DNS/network access, inspect HTTP headers, or
     * validate the downloaded font's contents. Those checks belong to other stages.
     *
     * @param url Parsed candidate request or redirect destination.
     * @param resource Expected resource kind; a URL approved for one kind is not
     *        automatically approved for another.
     * @return True only when all applicable URL rules pass. Unknown resource enum
     *         values and any rejected secure-URL/endpoint check return false.
     */
    bool allowsUrl(const QUrl &url, Resource resource) const override;
};
