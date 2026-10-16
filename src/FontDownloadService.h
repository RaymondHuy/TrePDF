#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include "IFontProvider.h"
#include <memory>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * @brief Downloads, validates, and installs an exact font family for TrePDF.
 *
 * This GUI-thread service owns one active operation at a time. A normal operation
 * resolves provider metadata, visits the available Regular/Italic/Bold/Bold Italic
 * faces, stages their validated bytes, and installs the new faces together through
 * ApplicationFonts. The face requested by the caller is processed first. Existing
 * usable cache entries can satisfy a face without another download; a cache hit
 * for the requested face can satisfy start() immediately without fetching siblings.
 *
 * Ordinary families use Google Fonts followed by the packaged CTAN catalogue when
 * a provider reports that the family is absent. Supported Aptos families instead
 * use the reviewed Microsoft archive. Providers describe resources and URL rules;
 * this service owns network requests, response limits, cancellation, and staging.
 *
 * The caller must obtain download consent before start(). Requests disclose font
 * names/style information, never PDF text or document paths. The required Unicode
 * code point is used locally for glyph validation and is not sent to a provider.
 *
 * Keep the service and its network manager on the GUI thread and invoke its
 * instance methods there. Network I/O is asynchronous, but parsing, validation,
 * and installation run in that thread. This class does not edit or save a PDF;
 * callers decide whether to resume their pending operation after finished().
 */
class FontDownloadService final : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates an idle service and configures its providers and deadline.
     *
     * Registers Google Fonts before CTAN and sets a single-shot, 120-second
     * deadline for an entire operation. Construction does not start the timer,
     * issue requests, or install fonts. The deadline reports a localized timeout
     * through the same completion path as other failures.
     *
     * @param parent Optional QObject owner of this service.
     * @param network Optional manager used for requests, primarily for deterministic
     *        tests. It is borrowed, is not reparented, must have compatible thread
     *        affinity, and must outlive the service. Passing nullptr creates a
     *        QNetworkAccessManager owned by this service.
     */
    explicit FontDownloadService(QObject *parent = nullptr,
                                 QNetworkAccessManager *network = nullptr);

    /**
     * @brief Silently abandons any pending operation before QObject teardown.
     *
     * Calls cancel() to stop the deadline, disconnect and abort the active reply,
     * discard staged bytes, and invalidate queued installation callbacks. No
     * finished() signal is emitted. A borrowed network manager is not destroyed;
     * a manager created by the constructor is released by QObject ownership.
     */
    ~FontDownloadService() override;

    /**
     * @brief Starts a consented download, replacing any unfinished operation.
     *
     * First cancels the previous operation silently. The family is trimmed and a
     * PDF subset prefix of six uppercase ASCII letters followed by '+' is removed
     * when followed by a nonempty family name. The normalized name must be nonempty,
     * at most 128 UTF-16 code units, and free of ASCII control characters and DEL.
     * Invalid input completes with failure.
     *
     * A usable cached copy of the requested face completes successfully at once;
     * this fast path does not fill in missing sibling styles. Otherwise, supported
     * Aptos names are canonicalized and downloaded as one archive, while other
     * names enter provider lookup. Available sibling faces are also collected, but
     * unavailable styles are never synthesized. Newly downloaded faces remain in
     * memory until the whole selected batch is ready for installation.
     *
     * The 120-second operation deadline spans lookup, downloads, mirror retries,
     * and the queued installation step; advancing a stage does not restart it.
     * Most work completes later through the event loop. However, invalid input,
     * a cache hit, or an immediate lookup/request rejection can emit finished()
     * before start() returns. Connect completion handlers before calling start().
     *
     * @param family Requested family name, optionally with a PDF subset prefix.
     * @param bold Whether the face needed by the pending edit/save must be bold.
     * @param italic Whether that face must be italic.
     * @param requiredCodePoint Unicode scalar that the requested face must cover.
     *        Zero disables this additional character check. Values above U+10FFFF
     *        and surrogate code points are rejected. Sibling faces are not required
     *        to cover this particular character, but still undergo normal checks.
     * @pre The caller has obtained the user's consent for this family download.
     * @pre Called on the service's GUI thread; the implementation asserts affinity.
     */
    void start(const QString &family, bool bold, bool italic, uint requiredCodePoint);

    /**
     * @brief Returns the translated display label for one of the four face styles.
     *
     * The flag combinations select Regular, Bold, Italic, or Bold Italic. Labels
     * use the current application translations and are intended for UI messages,
     * including failure details and lists of available styles. They are not stable
     * identifiers and must not be used as provider keys or persisted style codes.
     *
     * @param bold Selects the bold component of the style.
     * @param italic Selects the italic component of the style.
     * @return Localized style label; no service instance or network request is needed.
     */
    static QString styleName(bool bold, bool italic);

    /**
     * @brief Cancels the current operation without reporting success or failure.
     *
     * Advances the operation generation, marks the service inactive, and stops the
     * deadline. Any current reply is detached from the service, disconnected,
     * aborted, and scheduled for deletion. Buffered response data, staged faces,
     * and provider/mirror progress are discarded. Generation checks prevent an
     * installation callback queued by an older operation from committing later.
     *
     * Safe to call while idle or repeatedly. Cancellation emits no finished()
     * signal and does not uninstall fonts already present in ApplicationFonts.
     * It also cannot interrupt synchronous installation already executing on the
     * GUI thread; scheduleInstallation() provides an event-loop cancellation point
     * before that commit begins.
     */
    void cancel();

signals:
    /**
     * @brief Reports the terminal result of an active, non-cancelled operation.
     *
     * Emitted after complete() has stopped the timer, detached the reply, and
     * cleared operation state. A handler may therefore start another operation.
     * There is at most one terminal emission for an operation; explicit cancel(),
     * replacement by start(), and destruction intentionally produce none.
     *
     * @param success True when the requested face was already usable in the cache,
     *        or when all staged faces were successfully installed. This does not
     *        mean that a pending PDF edit/save has itself completed.
     * @param error Empty on current success paths. On failure, a localized message
     *        containing the family, the relevant face style, and the failure reason.
     *        Whole-batch installation failures identify the caller's requested style.
     * @note May be emitted synchronously during start(); see its connection guidance.
     */
    void finished(bool success, const QString &error);

private:
    /// Response kind: selects URL policy, byte limit, and the successful-response parser.
    enum class Stage { Metadata, Stylesheet, Font, AptosArchive };

    /**
     * A real face in the current family batch. Nonempty data contains validated,
     * uninstalled font bytes. Empty data may mean either an unprocessed face or a
     * face satisfied by the cache; installFamily() rechecks the latter explicitly.
     */
    struct Face {
        bool bold = false;
        bool italic = false;
        QByteArray data;
    };
    /**
     * @brief Issues the next GET request and connects its bounded receive handlers.
     *
     * Requires an active operation and an available network manager. A normal URL
     * must pass allowedUrl(); rejection terminates the operation. The caller must
     * have released the preceding reply before requesting another resource, since
     * the service tracks only one reply. This method replaces the response buffer
     * and current stage, but does not restart the overall operation deadline.
     *
     * Requests use manual redirects, manual cookie loading/saving and authentication
     * reuse, identity content encoding, and a legacy User-Agent used by the font
     * source endpoints. Each request has a 15-second transfer timeout and a 256 KiB
     * read buffer. readyRead/finished callbacks retain the reply identity so that
     * cancelled or superseded replies cannot advance the active operation.
     *
     * @param url Fully resolved endpoint to fetch.
     * @param stage Expected response kind, retained for validation and dispatch.
     * @param approvedRedirect True only after receiveFinished() has checked the
     *        redirect with the provider's redirect policy or the Aptos allowlist.
     *        Skips the initial URL check because an approved redirect destination
     *        may differ from the provider's permitted initial endpoints.
     */
    void request(const QUrl &url, Stage stage, bool approvedRedirect = false);

    /**
     * @brief Advances to the next provider after an absent-family result.
     *
     * Resets redirect and mirror progress, then requests that provider's metadata.
     * An empty metadata URL denotes a packaged local catalogue and is resolved
     * immediately with an empty payload. Exhausting providers reports unavailable;
     * an inactive operation does nothing. Provider fallback is for NotFound, not
     * malformed metadata, missing required styles, or arbitrary network failures.
     */
    void nextProvider();

    /**
     * @brief Maps transport stages to the resource categories understood by providers.
     * @param stage Stage whose URL/redirect policy is being evaluated.
     * @return Metadata or Stylesheet for those stages, otherwise Font. AptosArchive
     *         maps to Font here, although its URL checks use a separate allowlist.
     */
    static IFontProvider::Resource resourceForStage(Stage stage);

    /**
     * @brief Appends available reply bytes without exceeding the stage's size cap.
     *
     * Ignores calls for an inactive operation or a reply other than m_reply. Checks
     * both the advertised Content-Length and the bytes currently available against
     * the remaining buffer allowance before readAll(). An excessive response ends
     * the whole operation with the localized download-too-large error. This handler
     * only buffers bytes; parsing waits until receiveFinished().
     *
     * @param reply Non-null emitting reply; ownership remains with Qt's reply lifecycle.
     */
    void receiveData(QNetworkReply *reply);

    /**
     * @brief Finalizes one response, follows approved redirects, or advances parsing.
     *
     * Checks reply identity, drains final bytes through receiveData(), then detaches
     * and schedules deletion of the completed reply before starting any next request.
     * Redirects require a 3xx response, provider/Aptos approval, no '.' or '..' path
     * segments in the original Location, and no more than three redirects in the
     * current chain. Relative destinations are resolved against the reply URL.
     *
     * Without a redirect, only HTTP 200 with NoError is accepted. Transport failures,
     * absent HTTP status, 403/404/408/429, and 5xx responses may attempt a reviewed
     * direct-font mirror; other failures, or exhausted mirrors, report a network
     * error. Unsafe redirects and invalid font data never trigger mirror fallback.
     *
     * Successful bodies go to the metadata, stylesheet, or archive parser. A Font
     * body is validated into the current face, then the face index advances and
     * requestNextFace() continues. No face is installed directly by this callback.
     *
     * @param reply Non-null finished reply; stale replies are ignored.
     */
    void receiveFinished(QNetworkReply *reply);

    /**
     * @brief Resolves a provider's family metadata and prepares its available faces.
     *
     * Delegates decoding to the current provider. NotFound tries the next provider;
     * invalid/unusable metadata or an empty license fails immediately. A resolved
     * family supplies the canonical name, available styles, and whether newly
     * downloaded faces must pass the Vietnamese coverage checks.
     *
     * The requested style must actually be offered. Otherwise, completion explains
     * its absence and, when available, lists the provider's other styles. A valid
     * result creates the face queue and begins processing it with requestNextFace().
     *
     * @param data Complete metadata response, or empty bytes for a local catalogue.
     */
    void processMetadata(const QByteArray &data);

    /**
     * @brief Resolves a provider stylesheet to a font-file request.
     *
     * Asks the active provider to extract the font URL. An empty result fails as
     * unavailable; otherwise resets redirect accounting and requests Stage::Font,
     * whose URL is checked by request(). The stylesheet itself is never installed.
     *
     * @param data Complete stylesheet response for the current face.
     */
    void processStylesheet(const QByteArray &data);

    /**
     * @brief Reads the reviewed Aptos ZIP and stages matching faces without extraction.
     *
     * Writes the bounded response to an automatically removed temporary file for
     * minizip access. Archive entries are never extracted to their stored paths.
     * Only exact expected root filenames for the selected family are read: spaces
     * in the family become hyphens, followed by the appropriate .ttf style suffix.
     * Other entries are skipped after their declared sizes have been accounted for.
     *
     * Bounds include 128 entries, filenames shorter than 512 bytes, 16 MiB per
     * declared entry, and 64 MiB of total declared uncompressed data. Matching faces
     * must be unique, unencrypted, readable, and agree with their declared size;
     * closing each entry must also pass minizip's integrity check. A malformed
     * archive, I/O failure, or missing requested style ends the operation.
     *
     * Available matching styles form the batch. Cached faces are reused; all other
     * faces pass stageFont(). Only after staging succeeds is installation queued.
     *
     * @param data Complete Microsoft archive response, already limited to 8 MiB.
     */
    void processArchive(const QByteArray &data);

    /**
     * @brief Replaces the face queue with exactly the styles reported as available.
     *
     * Clears staged faces and resets the face index. The caller's requested face is
     * placed first when available, so its validation fails early if necessary.
     * Remaining faces follow Regular, Italic, Bold, Bold Italic order, excluding
     * the already queued requested face. No network or cache checks happen here.
     *
     * @param regular Whether a real Regular face is offered.
     * @param italic Whether a real Italic face is offered.
     * @param bold Whether a real Bold face is offered. Note that this parameter
     *        follows italic, unlike the bold/italic order used by start().
     * @param boldItalic Whether a real Bold Italic face is offered.
     */
    void prepareFaces(bool regular, bool italic, bool bold, bool boldItalic);

    /**
     * @brief Skips usable cached faces and requests the next missing face.
     *
     * Requires a resolved provider and face queue; does nothing while inactive.
     * When all faces are satisfied, queues installation rather than installing
     * inline, leaving an event-loop opportunity for cancellation. Otherwise sets
     * failure-message context to this face and chooses the primary URL or the
     * current mirror. A mirror selected earlier remains selected for later faces.
     *
     * Starts either Stylesheet or Font according to the provider's resource type
     * and resets the redirect count for this request chain. Cached faces remain
     * represented by empty staged data and are checked again before installation.
     */
    void requestNextFace();

    /**
     * @brief Attempts the next reviewed mirror for a failed direct-font request.
     *
     * Eligible only during an active Font stage with a valid provider/current face
     * and a provider that supplies font files directly. Metadata, stylesheet-based
     * providers, and Aptos archive requests cannot use this path. At most two
     * alternatives are visited across the family, within the original deadline;
     * the source index is not reset between styles.
     *
     * Called only after receiveFinished() classifies a failure as retryable.
     * Advances the source index and invokes requestNextFace(), without relaxing URL
     * or font validation. A selected mirror may serve the remaining styles too.
     *
     * @return True if an alternative was selected and processing handed off to
     *         requestNextFace(); this does not guarantee download success. False
     *         when ineligible or no permitted alternative remains.
     */
    bool retryFontFromMirror();

    /**
     * @brief Validates one downloaded face and retains its bytes for the batch.
     *
     * ApplicationFonts checks that the data is an acceptable editable font matching
     * the resolved family and exact bold/italic flags. A valid QRawFont must then
     * cover printable ASCII U+0020 through U+007E. The requested face additionally
     * needs requiredCharacter(face), when nonzero. If the family requires Vietnamese
     * coverage, each newly staged face must also cover the explicit accent set and
     * the U+1EA0 through U+1EF9 range used by the implementation.
     *
     * Success assigns face.data without writing files or registering fonts. A
     * validation/coverage failure sets the relevant face context and completes the
     * entire operation with a localized error, clearing its pending batch. Inactive
     * calls return false silently. Cached faces bypass this staging path.
     *
     * @param data Complete font-file bytes, bounded by the receive/archive path.
     * @param face In/out queue entry supplying style flags and receiving valid bytes.
     *        A failure can clear m_faces, so callers must not reuse a reference into
     *        that list after this method returns false.
     * @return True only when the bytes have been successfully staged.
     */
    bool stageFont(const QByteArray &data, Face &face);

    /**
     * @brief Checks whether ApplicationFonts already has this exact usable face.
     *
     * Looks up the current family and the face's bold/italic flags in the private
     * registry of stored fonts. A valid stored face is sufficient for siblings; the
     * requested face must also support its nonzero required code point. This is not
     * a system font lookup and never accepts a visually similar fallback or
     * synthesized style.
     *
     * The cache path does not repeat stageFont()'s printable-ASCII/Vietnamese loops.
     * It relies on the registry's validated stored face and performs only the extra
     * requested-character check here. It does not mutate the queue or install data.
     *
     * @param face Style to check; its staged data field is not consulted.
     * @return True when the stored face satisfies these cache-path requirements.
     */
    bool hasCachedFace(const Face &face) const;

    /**
     * @brief Defers the installation commit to the next event-loop opportunity.
     *
     * Captures m_generation in a zero-delay callback bound to this QObject. The
     * callback installs only while the service is active and the generation still
     * matches. cancel(), a replacement start(), or destruction therefore prevents
     * an obsolete callback from installing a discarded batch. This is a deliberate
     * cancellation boundary after staging, not a worker-thread installation job.
     */
    void scheduleInstallation();

    /**
     * @brief Revalidates batch readiness and atomically installs its staged faces.
     *
     * Does nothing while inactive and rejects an empty face list. Every entry with
     * empty data must still have a usable cached face; losing one before commit
     * fails the operation. Nonempty entries become one ApplicationFonts installation
     * request containing bytes, family/style identity, and per-face required glyphs.
     *
     * ApplicationFonts owns validation, persistent storage, and registration of that
     * batch. Its transaction must succeed as a unit; no partial family is installed
     * between network requests. If there are no new bytes, the verified cached batch
     * succeeds without invoking the installer. An installation failure reports the
     * caller's requested style because the transaction cannot identify one sibling
     * as its sole cause. Both outcomes terminate through complete().
     */
    void installFamily();

    /**
     * @brief Selects the caller's extra glyph requirement for a particular face.
     * @param face Face whose bold and italic flags are compared with the request.
     * @return m_requiredCodePoint only for the exact requested style; zero for every
     *         sibling. Zero means no extra per-request glyph constraint, not a
     *         requirement to contain a U+0000 glyph.
     */
    uint requiredCharacter(const Face &face) const;

    /**
     * @brief Completes with the generic unavailable-font-style explanation.
     *
     * Used when lookup, URL policy, archive structure, or batch readiness cannot
     * provide an acceptable face and no more specific message is selected. Delegates
     * to complete(false, ...), which adds family/style context and cancels pending
     * work. Calling it after an operation has ended emits nothing.
     */
    void failUnavailable();

    /**
     * @brief Ends the active operation and emits its result exactly once.
     *
     * Returns immediately if already inactive. On failure, captures the current
     * family and error-face style in a translated message before cancel() clears
     * pending state. Then performs the same cleanup as cancellation and emits
     * finished(). Cleanup precedes notification, allowing a handler to reuse the
     * service without an old reply or queued installation affecting the new work.
     *
     * @param success Whether cache reuse or installation succeeded.
     * @param error Localized failure reason to wrap with family/style context.
     *        For success it is passed through unchanged; current callers use empty.
     */
    void complete(bool success, const QString &error);

    /**
     * @brief Checks whether a URL is permitted as an initial request for a stage.
     *
     * All URLs must be valid HTTPS URLs with a nonempty host, no user information,
     * no explicitly specified port, and no fragment. Aptos archives additionally
     * require download.microsoft.com, a path starting with /download/ and ending
     * with .zip, and no query. Other stages require a current provider and its
     * allowsUrl() approval for resourceForStage(stage).
     *
     * Redirects can have a separate provider policy; receiveFinished() evaluates
     * that policy before calling request() with approvedRedirect set to true.
     *
     * @param url Candidate endpoint; this method neither resolves nor fetches it.
     * @param stage Resource kind used to select the allowlist.
     * @return True only if the applicable secure-URL and source rules accept it.
     */
    bool allowedUrl(const QUrl &url, Stage stage) const;

    /**
     * @brief Returns the maximum buffered response-body size, in bytes, for a stage.
     *
     * Limits are 1 MiB for metadata, 128 KiB for stylesheets, 16 MiB for one font,
     * and 8 MiB for the Aptos ZIP response (MiB/KiB use powers of 1024). These caps
     * are applied by receiveData() to both advertised and accumulated data. Archive
     * decompression and installed-font storage have additional independent limits.
     *
     * @param stage Expected response kind.
     * @return Stage limit, or zero for a value outside the defined enum cases.
     */
    static qint64 maximumBytes(Stage stage);

    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    QByteArray m_data;
    QString m_family;
    std::vector<std::unique_ptr<IFontProvider>> m_providers;
    qsizetype m_providerIndex = -1;
    IFontProvider::Family m_providerFamily;
    QList<Face> m_faces;
    qsizetype m_faceIndex = 0;
    quint64 m_generation = 0;
    Stage m_stage = Stage::Metadata;
    uint m_requiredCodePoint = 0;
    int m_redirects = 0;
    int m_fontSourceIndex = 0;
    bool m_bold = false;
    bool m_italic = false;
    bool m_errorBold = false;
    bool m_errorItalic = false;
    bool m_requireVietnamese = false;
    bool m_active = false;
};
