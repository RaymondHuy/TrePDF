# Hướng dẫn toàn bộ tệp C++

Tài liệu này mô tả 18 tệp triển khai C++ trong `src/`, 20 header và một tệp kiểm
thử C++ trong `tests/`. Trong số header, 17 tệp đi cùng implementation và ba tệp
hỗ trợ không có `.cpp` riêng. Nội dung tập trung vào trách nhiệm thật của mỗi
tệp, ownership, thread, dữ liệu trao đổi và ranh giới không nên phá khi sửa code.

## Quy ước kiến trúc C++

- Controller và `PDFDocument` sống trên GUI thread; QML chỉ gọi API Qt/QML được
  expose, không truy cập handle PDFium trực tiếp.
- PDFium không thread-safe ngay cả khi dùng các document handle khác nhau. Mọi
  transaction PDFium phải đi qua `PDFiumProcessLock` toàn tiến trình.
- Render, search, form runtime và save mở document handle riêng; không dùng
  `FPDF_DOCUMENT` thuộc `PDFDocument` trên worker.
- Mọi kết quả bất đồng bộ phải được kiểm request id/generation/document key/
  revision trước khi áp vào object GUI.
- Save request là snapshot bất biến, không giữ con trỏ controller/document sang
  worker.

## Khởi động, font và item hỗ trợ

### [`src/main.cpp`](../src/main.cpp)

**Vai trò:** entry point của ứng dụng.

- Tạo `QGuiApplication`, đặt application name `TrePDF` và organization
  `TreOffice`.
- Gọi `ApplicationFonts::registerBundledFonts()` trước khi QML tạo font control,
  để danh mục font và face do app bundle cung cấp ổn định ngay từ đầu.
- Ép Qt Quick Controls dùng style `Basic`, phù hợp các control QML tự vẽ nền/
  content item.
- Tạo `QQmlApplicationEngine`, đặt UI language mặc định là `vi`, xử lý lỗi tạo
  root object và load `Main` từ module `TrePDF`.
- Chạy event loop. Tệp này không khởi tạo document/controller thủ công; các type
  được QML module/plugin đăng ký từ khai báo QML element trong class tương ứng.

### [`src/ApplicationFonts.cpp`](../src/ApplicationFonts.cpp) và
[`src/ApplicationFonts.h`](../src/ApplicationFonts.h)

**Vai trò:** nguồn sự thật cho font có thể hiển thị và có thể lưu an toàn vào PDF.

**Khởi tạo registry:**

- Chụp danh mục system family/face trước khi đăng ký bundled fonts để phân biệt
  nguồn system với resource app dù tên family trùng nhau.
- Đăng ký font dưới `src/assets/fonts`, ghi nhận exact face theo family, bold và
  italic; bundled face được ưu tiên khi giải quyết font draft.
- Cache danh sách family đã lọc để font picker không chạy probe CoreText nặng cho
  mỗi lần mở menu.

**Kiểm tra font dùng khi Save:**

- Trên macOS, dùng CoreText lấy đúng face bytes thay vì suy đoán từ family name.
- Nếu nguồn là TTC/collection, trích face cần dùng thành sfnt độc lập để PDFium
  không chọn nhầm face.
- Chỉ chấp nhận static TrueType `glyf/loca`, kích thước tối đa 64 MiB và quyền
  embedding Installable hoặc Editable theo OS/2.
- Loại CFF/CFF2, variable, color/bitmap-only, face thiếu glyph, Restricted và
  Preview & Print-only; trả error có loại để Save dừng rõ ràng thay vì substitute.
- Kiểm coverage theo Vietnamese/English/Chinese/Japanese/Korean cho filter UI,
  đồng thời vẫn kiểm chính xác glyph của nội dung ở bước chuẩn bị save.

**Hiệu chuẩn hiển thị:**

- Đo mapping từ target PDF horizontal scale sang `QFont::setStretch()` vì stretch
  của Qt/CoreText không tuyến tính cho mọi face.
- Cache calibration theo family/style/pixel size để `EditOverlay` và
  `PDFSaveWriter` dùng cùng phép đo, tránh preview khác layout khi ghi.

**API dữ liệu:** `FontLanguage`, `FontFaceData`, danh sách bundled/system family,
font bytes, glyph/face probe và helper stretch. Không giữ handle PDFium.

### [`src/DraftImageItem.cpp`](../src/DraftImageItem.cpp) và
[`src/DraftImageItem.h`](../src/DraftImageItem.h)

**Type QML:** `DraftImageView`, kế thừa `QQuickPaintedItem`.

- Expose property `sourceImage` dạng `QVariant`; chấp nhận snapshot `QImage` do
  controller đã decode và giới hạn kích thước.
- `paint()` vẽ ảnh với smooth transform vào bounds item, giữ việc preview độc lập
  với đường dẫn file gốc.
- Khi property đổi, phát signal và gọi `update()`.
- Không mở file ảnh, không sở hữu draft history và không nhúng ảnh vào PDF. Mục
  đích là bảo đảm preview/save dùng cùng pixel snapshot kể cả file nguồn bị đổi
  hoặc xóa sau khi người dùng chọn.

## Tài liệu, tab và render

### [`src/PDFDocument.cpp`](../src/PDFDocument.cpp) và
[`src/PDFDocument.h`](../src/PDFDocument.h)

**Vai trò:** model document trên GUI thread và chủ sở hữu main
`FPDF_DOCUMENT`.

**Mở/đóng:**

- Mở local file bằng `FPDF_LoadDocument()` từ path, không đọc toàn bộ PDF vào RAM.
- Quản lý PDFium process lifetime, password/error status và đóng handle đúng
  thread.
- Cache kích thước mọi source page để QML gọi `pageSize()` nóng mà không chạm
  PDFium lặp lại.
- Expose source URL, loaded/status, page count/current page, current/max page size.

**Cấu trúc trang transient:**

- Giữ page map hiển thị gồm source page và blank page chèn tạm; giữ danh sách
  source page đã đánh dấu xóa.
- `PDFPageInsertion` chứa vị trí/kích thước cần writer tái tạo khi Save.
- Cung cấp ánh xạ display page ↔ source page cho renderer, edit và form.
- Page insert/delete chỉ thay state session và revision; không sửa main document
  handle tại chỗ.

**Revision:** tăng khi document underlying hoặc cấu trúc render thay đổi. Mọi
worker result mang revision cũ phải bị loại.

**Ranh giới:** handle này chỉ dùng trên GUI thread. Worker lấy
`renderFilePath()` và tự mở handle riêng.

### [`src/PDFTabModel.cpp`](../src/PDFTabModel.cpp) và
[`src/PDFTabModel.h`](../src/PDFTabModel.h)

Tệp định nghĩa ba lớp có ownership khác nhau.

#### `PDFViewState`

- Là state object QML-anonymous cho một pane phụ, gồm zoom, scroll X/Y, current
  page và cờ `positionInitialized` để phân biệt vị trí `(0, 0)` hợp lệ với pane
  chưa từng được khởi tạo.
- Clamp zoom/page/scroll trước khi phát signal; `setScrollPosition()` cập nhật hai
  trục như một thao tác để workspace lưu/khôi phục viewport nhất quán.

#### `PDFDocumentSession`

- Sở hữu `PDFDocument`, `PDFEditController`, `PDFFormController`,
  `PDFFormDesignerController` và `PDFSearchController` của một tab.
- Nối document vào bốn controller, nối form/designer vào edit controller để draft
  guard và save snapshot bao quát cả content lẫn AcroForm. Search state không
  tham gia draft/save và được giữ độc lập theo tab.
- Giữ state view chính: zoom, scroll X/Y, quarter-turn rotation, bốn mode Page
  Display, cờ trang bìa riêng, panel collapsed và cờ initial fit.
- Expose `SplitViewMode` gồm No/Vertical/Horizontal cùng một child
  `PDFViewState` để pane phụ giữ zoom, trang và vị trí cuộn độc lập.
- Tổng hợp `hasDrafts`/`saving`, title/path và chuyển tiếp status, save finished,
  external-link request cho model/QML.
- `prepareForDeactivation()` kết thúc edit/focus form trước khi QML phá workspace.

#### `PDFTabModel`

- Là `QAbstractListModel` của session, expose title/session/draft/saving roles cho
  `DocumentTabBar`.
- `open()` chuẩn hóa canonical path và tái kích hoạt tab hiện có thay vì mở cùng
  tệp hai lần.
- Quản lý current index/session, đóng session, Save/Save As đúng tab và tìm tab
  đầu tiên còn draft cho workflow Quit.
- Chặn Save As ghi đè đường dẫn đang được tab khác dùng; xóa identity khi
  document đóng.
- Tổng hợp `anySaving` và `hasDrafts` toàn ứng dụng để `Main.qml` khóa hành động
  nguy hiểm.

**Ranh giới:** model không tự hiển thị prompt; nó trả trạng thái/signal để QML
điều phối lựa chọn người dùng.

### [`src/PDFSearchController.cpp`](../src/PDFSearchController.cpp) và
[`src/PDFSearchController.h`](../src/PDFSearchController.h)

**Type QML:** `PDFSearchController`, facade GUI-thread cho tìm text toàn tài liệu.

**State và API:**

- Expose query, `Idle/Searching/Ready/Limited/Failed`, tổng số kết quả, index hiện
  tại và `resultsRevision`; `prepareSearchText()`, `scheduleSearch()`, `search()`,
  `clearSearch()`, `nextResult()`, `previousResult()` và `highlightsForPage()` là
  API QML. Timer debounce 300 ms nằm trong controller nên không mất request khi
  `ViewRibbon` bị hủy hoặc đổi document tab.
- Kết quả lưu theo thứ tự display page/match. Previous/Next quay vòng và phát
  `resultActivated(page, normalizedX, normalizedY, resultsRevision)`; QML dùng
  điểm anchor và revision để cuộn đúng match vào giữa viewport thay vì chỉ đi
  tới đầu trang hoặc replay một match đã stale.
- Rect trả cho QML là normalized geometry của trang logic, kèm cờ `current`; UI
  không giữ hoặc tự suy ra PDF text geometry.

**Worker và giới hạn:**

- Mỗi query chụp path, document revision, ánh xạ display/source page và page
  size. Controller chỉ giữ snapshot pending mới nhất; `tryStart()` cùng timer
  retry không để request đã cancel tích tụ trong queue của pool một thread.
  Worker mở một `FPDF_DOCUMENT` riêng từ path và chỉ chạm PDFium dưới
  `PDFiumProcessLock`, với cancellation được kiểm lại ngay sau khi lấy lock.
- Dùng text page của PDFium để tìm không phân biệt hoa thường, gom rect của từng
  match và đổi PDF coordinates sang normalized top-left coordinates qua
  `FPDF_PageToDevice()`.
- Query tối đa 256 UTF-16 code units; mỗi page tối đa 2.000.000 text characters;
  toàn query tối đa 20.000 matches, 100.000 rectangles và 256 rectangles/match.
  QML chỉ materialize tối đa 512 rectangle trên mỗi page nhưng luôn ưu tiên
  current match. Cắt query không để lại high surrogate đơn lẻ. Scan không hoàn
  tất do cap/page lỗi hoặc thiếu page metrics dùng state `Limited`; text trên
  page thiếu metrics vẫn được đếm nhưng không tạo geometry không đáng tin.
- Query mới đặt cancellation token của request cũ. Callback chỉ nhận kết quả khi
  controller, token, path, revision và query vẫn khớp. `sourceChanged` hoặc
  `pageStructureChanged` hủy request và clear state. Session cũng clear search
  khi edit history đổi để highlight nguồn không nằm trên draft render đã khác.

**Ranh giới:** controller này đọc text layer nguồn, không dùng candidate cache
của `PDFEditController`, không tìm trong draft chưa Save và không giữ handle
PDFium trên GUI thread.

### [`src/PDFPageItem.cpp`](../src/PDFPageItem.cpp) và
[`src/PDFPageItem.h`](../src/PDFPageItem.h)

**Type QML:** `PDFPageView`, kế thừa `QQuickItem`, bật `ItemHasContents`.

**Lập lịch render:**

- Theo dõi document, edit/form/designer controller, page, zoom, color và kích
  thước item/DPR.
- Tạo render key gồm request id, pointer document, render revision, display/source
  page, pixel size, zoom, DPR, draft/form/designer revision.
- Debounce thay đổi draft ngắn bằng timer để thao tác kéo không tạo hàng đợi render
  vô hạn; request cũ có atomic cancellation flag.
- Gửi việc qua hàng đợi render có giới hạn của `PDFFormWorker`, nhờ đó render và
  form cùng dùng session document tuần tự thay vì đụng handle.

**Render transaction:**

- Với blank page, chỉ tạo ảnh nền.
- Với source page, mở page qua form worker và giữ một `PDFiumProcessLock` cho toàn
  bộ render/mutate-tạm/restore.
- `ScopedPageObjectSuppression` ẩn source object đã bị draft thay/xóa.
- `ScopedPageObjectMoves` áp ma trận preview cho source text/image chỉ-move rồi
  phục hồi object.
- `PDFWidgetAnnotationSuppression` ẩn source widget đang được form designer vẽ
  hoặc Dropdown đang giữ tombstone xóa draft; capture nền vùng widget, cho
  `FPDF_FFLDraw` chạy để form khác vẫn đúng, rồi dán nền lại trên widget bị
  suppress.
- Reply comment có `/IRT` được tạm đặt Hidden cùng Popup liên kết trong mỗi
  transaction render và phục hồi nguyên flags sau đó; panel và Save vẫn giữ
  đầy đủ reply, còn viewer chỉ vẽ comment gốc.
- Chỉ tái sử dụng form document nếu mọi object transform, annotation flag và patch
  đều phục hồi thành công.

**Nhận kết quả và scene graph:**

- Callback giữ `QPointer<PDFPageItem>` và kiểm lại toàn bộ render key. Kết quả cũ
  chỉ bị bỏ, không thay texture hiện hành.
- `updatePaintNode()` tạo texture từ pending `QImage`, dùng
  `QSGSimpleTextureNode::setOwnsTexture(true)` và xóa bản CPU ngay sau upload.
- Không gọi `setTexture(nullptr)` và không delete thủ công texture đã được node
  sở hữu.
- Expose `draftTextureCurrent`/`draftTextureReady` để `EditOverlay` biết khi nào
  cần tự vẽ preview hoặc mask texture sạch.
- Expose thêm `displayedSuppressedCommentObjectIds`, là snapshot chính xác các
  source comment đã bị ẩn trong texture được accept. Snapshot được publish trước
  scene-graph sync và ảnh pending tương ứng không bị bỏ bởi một draft revision
  mới, nhờ đó QML không bao giờ ghép preview của revision mới với texture của
  revision cũ theo cách vẽ source/draft hai lần.

**Ranh giới:** mọi thay đổi file này phải test native Metal, scroll/pool nhanh,
zoom, đóng/mở khi render đang chạy và shutdown render thread.

## Edit controller và writer nội dung

### [`src/PDFAnnotationUtils.cpp`](../src/PDFAnnotationUtils.cpp) và
[`src/PDFAnnotationUtils.h`](../src/PDFAnnotationUtils.h)

**Vai trò:** contract dùng chung cho comment annotation giữa candidate index,
render suppression và save cascade. Utility đọc chuỗi annotation với giới hạn
4 MiB, chuẩn hóa CR/LF, phân loại editable/full markup (kể cả `/Projection`) và
nhận diện quan hệ reply cùng annotation-state `/StateModel /Review` hoặc
`/Marked`. Các hàm chỉ mượn annotation handle, yêu cầu caller đã
giữ `PDFiumProcessLock` và không tự đóng handle hoặc acquire lock.

### [`src/PDFEditController.cpp`](../src/PDFEditController.cpp) và
[`src/PDFEditController.h`](../src/PDFEditController.h)

**Vai trò:** trung tâm nghiệp vụ của Edit/Comment: khám phá nội dung và
annotation, selection, draft, rich text, history, page operation và
orchestration Save.

**Khám phá candidate:**

- Scan page bất đồng bộ từ document handle riêng dưới PDFium lock.
- Duyệt top-level page objects và đường dẫn object con để nhận diện text/image;
  chỉ đánh dấu editable cho loại có thể lưu mà vẫn bảo toàn scope/resource.
- Đọc Highlight, Squiggly, Underline, Strikeout, Text, FreeText, Stamp, Square,
  Circle, Line, Polygon, PolyLine và Ink annotation;
  chuẩn hóa quad/rect, màu và `/Contents` thành comment candidate. Nội dung
  annotation nguồn được sửa trong panel bằng draft riêng. Cả sáu drawing subtype
  Square, Circle, Line, Polygon, PolyLine và Ink nguồn hợp lệ cho move/resize trên
  canvas bằng cách transform endpoint, vertex hoặc toàn bộ stroke tương ứng.
  Annotation nguồn ngoài sáu subtype này chưa cho move/resize;
  ReadOnly/LockedContents không nhận mutation và Locked chặn thay đổi geometry
  drawing comment.
  Xóa tạo tombstone draft theo annotation index và subtype.
- Khi text markup có explicit `/AP`, lấy fill/stroke RGBA từ path trong normal
  appearance nếu PDFium không trả annotation color; AP là nguồn hiển thị có
  thẩm quyền, không để `/C` hoặc `/CA` stale đổi màu model sau reopen.
- Đọc quan hệ reply chuẩn qua `/IRT` và `/RT`; chuỗi reply nhiều tầng từ PDF
  ngoài được lần về root rồi trình bày thành đúng một cấp. Metadata private chỉ
  dùng để định danh ổn định parent trong pipeline ghi trước khi tạo `/IRT` chuẩn.
- Nhận diện annotation `/Text` có parent `/IRT`, `/State` và `/StateModel`
  `/Review` hoặc `/Marked` như log chuyển trạng thái. Candidate vẫn nằm trong
  graph nội bộ để bảo vệ ancestor/cascade xóa, nhưng bị loại khỏi comment
  model, reply count và các API chọn/mutation trực tiếp. Controller canonical
  hóa exact spelling của reviewer theo history hiện hữu, chọn terminal sâu nhất
  của chuỗi `/IRT` và chỉ dùng `/M`/index để phân xử nhánh rẽ. Hai API
  `setCommentStatus()` và `setCommentMarked()` tạo các draft độc lập có
  undo/redo/discard.
- `requestCommentIndex()` dùng worker riêng để quét annotation-only từng trang,
  nhả PDFium lock giữa các trang và chỉ giữ comment candidate. Vì vậy danh sách
  toàn tài liệu không làm phình cache text/image 12 trang của viewport.
- Ink hợp lệ được đọc thành các normalized stroke riêng, không flatten
  `/InkList`. Scanner giới hạn 64 stroke, 4.096 điểm mỗi stroke và 32.768 điểm
  tổng; stroke phải có ít nhất hai điểm hữu hạn. Ink malformed hoặc oversized
  hạ xuống kind `comment` generic chỉ đọc để panel vẫn điều hướng mà không cho
  mutation hay cấp phát dữ liệu không giới hạn.
- Gom các text object tương thích trên cùng visual line thành một candidate nhưng
  giữ mọi source object path để suppress/remove chính xác.
- Trích source bounds, baseline/origin, font family/style/size/stretch, render
  mode/color, character span và paragraph format.
- Giữ geometry chuẩn hóa của từng glyph source-text không phải whitespace trong
  kết quả scan/cache theo page và revision. Bốn text-markup tool hit-test trên dữ liệu này;
  GUI/QML không mở `FPDF_PAGE`/`FPDF_TEXTPAGE` hay gọi PDFium để dò chữ.
- Text có render mode Invisible, thường là OCR ẩn trên trang scan, được giữ dưới
  dạng record nội bộ chỉ phục vụ hit-test text markup và bị loại khỏi
  `candidatesForPage()`/`candidateAt()`, nên không thể select/edit như source
  text thường. Geometry axis-aligned hiện chỉ nhận glyph ngang/upright (góc gần
  0°); glyph xoay/dọc bị bỏ qua cho tới khi pipeline hỗ trợ oriented QuadPoints.
- Mỗi page chỉ cache per-glyph text-markup geometry khi text layer có tối đa 50.000
  ký tự. Nếu vượt safety cap này, scan không giữ partial glyph geometry nào cho
  page đó để giới hạn RAM; candidate/source text editing vẫn được quét như cũ.
- Cache candidate theo page/revision; hover có đường try-lock/defer để không khóa
  GUI khi PDFium đang bận.

**Draft model và history:**

- Draft chứa kind, source ids/paths, text, character spans, paragraph formats,
  immutable source rect, layout frame, content bounds, image snapshot, shape,
  text-markup/sticky-note geometry, các stroke Ink, common drawing properties
  hiện hành và insertion order tùy loại.
- Mỗi action logic tạo snapshot command: add, delete, move, resize, format, page
  insert/delete. Undo/redo phục hồi toàn bộ state liên quan, không chỉ text.
- `CommentSelectTool` dùng cùng selection API với `SelectTool` nhưng
  `candidateAt()`, `selectCandidate()` và guard trung tâm `setSelection()` chỉ
  nhận comment kind. Đổi sang tool này sẽ xóa selection ngoài comment; restore
  từ undo/redo cũng đi qua guard nên không thể hồi sinh text/image/Edit shape.
- API selection tách capability property khỏi move/resize/delete. Stroke,
  interior và opacity tạo từng snapshot command riêng; no-op không thêm history.
  Source style chỉ bật cho cả sáu drawing subtype TrePDF-managed có normal AP
  và style đã capture an toàn; fingerprint AP bounded được giữ để Save xác thực.
- `hasDrafts` bao gồm edit history, transient page structure, form runtime draft
  và form designer changes.
- `discardDrafts()` phục hồi session state nhưng không ghi document.

**Text edit:**

- Quản lý selection range UTF-16, typing format và các signal để QML merge format
  vào `QTextDocument` đang chạy.
- Tạo frame ban đầu từ Qt text width; khi mở editor, tính frame mở rộng transient
  tới candidate cùng hàng kế tiếp hoặc margin suy ra.
- Phân biệt no-op editor session với thay đổi thật; frame mở rộng chỉ thành draft
  khi text/alignment/resize thay đổi.
- Font picker lấy intersection exact faces theo mọi style span đang chọn; đổi
  family reset inherited stretch cho range thay đổi.

**Công cụ khác:**

- Add Image decode/giới hạn ảnh trên GUI thread rồi lưu pixel snapshot.
- Add Shape chuẩn hóa geometry và snap line/arrow gần trục. Khi kết thúc gesture
  Highlight/Underline/Squiggly/Strikeout, controller lấy các glyph
  không-whitespace giao vùng kéo, nhóm theo
  visual line rồi tạo rectangle/QuadPoints đã snap cho từng dòng. Không có glyph
  khả dụng thì không tạo draft. Nếu cache glyph chưa sẵn sàng khi thả chuột,
  controller giữ pending request cùng revision/page/selection/tool/màu, ưu tiên
  scan page đó và tự tạo đúng subtype sau khi geometry về; Discard xóa cả pending request.
  OCR ẩn vẫn dùng được, nhưng page không có text
  layer khả dụng, chỉ có glyph xoay/dọc hoặc vượt safety cap 50.000 ký tự thì
  không dùng được text markup. Sticky Note giữ frame 24 pt. Free Text nhận click
  hoặc drag để tạo frame text box, dùng kích thước mặc định cho click; nội dung
  của cả hai được chỉnh từ `CommentPanel`.
- Ink đi qua `addDrawingComment()` nhưng giữ geometry dạng mảng stroke. Một
  Pencil drag gửi tối đa 4.096 raw points; controller kiểm kiểu/finite, clamp,
  bỏ điểm gần trùng, từ chối nét quá ngắn, rồi simplify RDP với tolerance khoảng
  0,75 PDF point. Nếu vẫn quá lớn, tolerance tăng có giới hạn và output bị
  hard-cap ở 1.024 điểm. Một drag tạo một annotation/stroke; model vẫn hỗ trợ
  nhiều stroke khi rehydrate `/InkList` nguồn.
- Move/resize cả sáu drawing subtype dùng frame draft chung với
  text/image/shape. Move giữ kích thước và áp cùng delta vào endpoint, vertex
  hoặc stroke; resize áp scale ngang/dọc của frame vào toàn bộ geometry. Riêng
  `inkStrokesForDraft()` biến đổi từng stroke mà không flatten `/InkList`.
  Source appearance bị suppress trong preview, còn mỗi gesture hoàn tất thành
  đúng một command undo/redo.
- `commentItems`, `selectComment` và `selectRelativeComment` cấp model/điều
  hướng phẳng cho panel theo thứ tự root rồi replies. `setCommentText()` định
  danh bằng `page + objectId`, còn nội dung comment/reply nguồn hoặc mới đều tham
  gia cùng snapshot undo/redo. `setCommentText()` và `addCommentReply()` cùng
  enforce tối đa 700 UTF-16 code unit; `addCommentReply()` chỉ nhận root.
- Annotation-only scan giữ kind riêng cho
  Line/Square/Circle/Polygon/PolyLine/Ink/Stamp. Stamp nguồn hợp lệ cho
  drag/phím mũi tên, resize tám handle, sửa nội dung, reply và xóa; geometry
  dùng chung frame draft và history undo/redo/discard. Save vá `/Rect` sau
  serialization để giữ nguyên `/AP` và `/BBox`. Caret/FileAttachment/Sound/
  Redact/Projection dùng kind `comment` chỉ đọc. Mọi đường sửa, xóa và reply
  đều kiểm `commentTextReadOnly` trước khi tạo draft.
- Move/delete top-level source text/image; bảo vệ object lồng trong Form XObject.
- Insert/delete page transient với điều kiện tài liệu luôn còn ít nhất một trang.

**Save orchestration:**

- Kết thúc editor, lấy form snapshot, gọi `PDFSaveWriter::prepare()` trên GUI
  thread rồi chạy `save(request)` trên worker.
- Đặt `saving` để khóa edit/tab operations, chuyển error thành status dịch được.
- Sau commit thành công, mở lại path; chỉ khi reopen thành công mới xóa draft và
  history. Nếu reopen thất bại, đóng document stale vì object path không còn khớp.

### [`src/PDFSaveWriter.cpp`](../src/PDFSaveWriter.cpp) và
[`src/PDFSaveWriter.h`](../src/PDFSaveWriter.h)

**Vai trò:** serializer giao dịch từ snapshot của controller sang PDF đích.

**API:**

- `prepare(controller, target, optionalSourceOverride)` tạo `Request` bất biến.
- `save(request)` chạy không cần controller; overload `save(controller, target)`
  dành cho call đồng bộ/test.
- `Result` trả enum lỗi, chi tiết, page và code point không hỗ trợ để controller
  tạo thông báo phù hợp.

**Snapshot chuẩn bị ở GUI thread:**

- Sao chép source/target path, deleted pages, page insertions, mọi draft và form
  designer save fields.
- Snapshot `QImage`, font bytes và rich-text format; worker không mở lại đường dẫn
  ảnh hay gọi CoreText/QML object.

**Áp nội dung:**

- Mở writer document riêng từ source path và resolve toàn bộ source object path
  trước khi remove bất kỳ object nào.
- Thực hiện delete/insert page, giữ mapping từ display page về source page.
- Với source text/image chỉ đổi vị trí, dịch ma trận object gốc để giữ font/pixel,
  orientation và z-order.
- Với text đổi nội dung/format/frame, remove source objects và reflow bằng
  `QTextLayout`; tạo object theo line/style run, map top-left Qt sang bottom-left
  PDF và vẽ underline bằng path.
- Reuse source font hoặc PDF standard font khi an toàn; nếu cần thì load bundled/
  system TrueType snapshot, giữ font handle sống đến sau GenerateContent.
- Bảo toàn exact source horizontal scale khi reuse; chỉ tính width compensation
  cho font mới/standard.
- Tạo bitmap object cho ảnh thêm mới, path object cho sáu shape, các standard
  Highlight/Squiggly/Underline/Strikeout annotation với quad/color/Print flag,
  Text annotation, FreeText annotation và Stamp với rect/`Contents`/`NM`/Print
  flag. Stamp snapshot giữ preset `/Name`, artwork text và màu; canonicalizer
  gắn normal `/AP` vector mà không regenerate page content.
  Cả bốn text-markup subtype ghi quad từ các rectangle dòng đã snap. Sau khi
  serialize staging file, canonicalizer thêm
  normal `/AP` vector cho text markup mới mà không regenerate page content.
- Tạo drawing comment qua Square placeholder, snapshot common properties và
  map normalized geometry bằng `FPDF_DeviceToPage()`; canonicalizer đổi subtype,
  ghi `/L`/`/Vertices`, dictionary style và vector `/AP` cuối cùng.
- Ink dùng cùng placeholder transaction nhưng map từng điểm trong từng stroke
  bằng `FPDF_DeviceToPage()`. Request giữ các stroke riêng; canonicalizer ghi
  `/Subtype /Ink`, `/InkList`, `/BS`, `/C`, `/CA`, Print flag và `/AP` vector
  có round cap/join. Writer giới hạn 64 stroke, 4.096 điểm mỗi stroke và 32.768
  điểm tổng trước khi serialize.
- Với một trong sáu drawing comment nguồn đổi geometry, writer xác thực lại
  subtype, `/NM`, `/Contents`, annotation permission và các cờ
  ReadOnly/LockedContents/Locked; sau đó canonicalizer cập nhật `/Rect` cùng
  `/L`, `/Vertices` hoặc `/InkList` đã translate/scale trên annotation hiện có.
  Move của cả sáu subtype và mọi AP ngoài/custom giữ nguyên `/AP`; resize
  TrePDF-managed chỉ tái tạo AP khi properties được hỗ trợ và fingerprint AP
  nguồn vẫn khớp. Geometry-only không ghi đè `/Contents` hoặc `/RC`.
- Với source property edit, writer còn so normal-AP fingerprint với snapshot
  lúc index rồi chuyển cả original/current properties sang canonicalizer.
  Canonicalizer cập nhật `/C`, optional `/IC`, `/CA` và replacement `/AP` trong
  cùng transaction nhưng giữ nguyên `/BS`, `/BE`, `/LE` cùng key mở rộng.
- Sticky Note và reply mới được gắn normal `/AP` vector ngay trên writer
  annotation; source Sticky Note đang sửa mà thiếu AP cũng được bổ sung. Nội
  dung comment dùng LF trong model, chuẩn hóa thành CR khi ghi `/Contents`.
- Ghi parent/reply vào file tạm, định danh bằng metadata ổn định rồi canonicalize
  reply thành `/IRT <parent-ref>` và `/RT /R` trước khi commit giao dịch.
- Xóa comment nguồn, toàn bộ reply hậu duệ và `/Popup` liên kết theo annotation
  index từ lớn xuống nhỏ, xác thực lại subtype trước khi gỡ; page chỉ đổi
  annotation không gọi `FPDFPage_GenerateContent()`.
- Sửa comment nguồn tại chỗ trước bước xóa index: xác thực subtype, nội dung gốc,
  `/NM` nếu có và cờ bảo vệ; đổi `/Contents`, `/M` và `/RC` nếu tồn tại. Các
  subtype khác giữ `/AP`; FreeText thay appearance cũ bằng bản Unicode có wrap
  theo style hộp mặc định của TrePDF để không tiếp tục vẽ nội dung trước khi
  sửa. FreeText không có draft giữ nguyên appearance nguồn. `/Popup`, `/IRT`,
  `/RT`, geometry, màu và metadata khác được giữ nguyên.
- Giữ earliest source z-order bằng insert-at-index; object thêm mới theo insertion
  order. Dùng `FPDF_DeviceToPage()` để tôn trọng CropBox/rotation.
- Chỉ gọi `FPDFPage_GenerateContent()` cho page có content object thay đổi;
  page chỉ có comment/markup annotation không bị rewrite content stream.
- Với source paint mà PDFium không thể tái tạo an toàn, chuẩn bị rasterized paint
  có giới hạn; các mode/scope unsupported phải trả lỗi thay vì làm mất đồ họa.

**Commit và form:**

- Ghi intermediate/content document qua `QSaveFile` với
  `FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS`.
- Chuyển normalized designer field sang page coordinates và gọi
  `PDFAcroFormWriter` nếu có field cần ghi/normalize.
- Chỉ commit đích khi từng bước thành công; Save As không chạm source.

## Form runtime

### [`src/PDFFormController.cpp`](../src/PDFFormController.cpp) và
[`src/PDFFormController.h`](../src/PDFFormController.h)

**Vai trò:** facade form trên GUI thread; đây là object session mà QML và
`PDFFormView` được phép gọi.

**State expose:** interactive, has drafts/focus, accepts text input, focused page
và annotation, can undo/redo, cursor type, JavaScript enabled/pending.

**Gửi việc sang worker:**

- Tạo `PDFFormWorker::DocumentKey` từ session id, render path và revision.
- Chuyển pointer press/move/release/double click, key event, clipboard/edit
  command, page change, choice focus/selection và snapshot sang worker.
- Mỗi callback kiểm generation/key rồi mới cập nhật focus/cursor/history/render
  revision hoặc phát host action.

**PDF actions và sandbox:**

- Bật JavaScript form tự động và xử lý lifecycle/page action qua worker.
- Cho phép internal go-to điều hướng trong document.
- HTTP/HTTPS do tương tác người dùng tạo được chuyển thành external-link request
  để QML xác nhận; scheme/file/mail/print/submit/dialog/timer không hỗ trợ bị chặn.
- `confirmExternalLink()`/`cancelExternalLink()` dùng request id để không xác nhận
  nhầm popup cũ.

**Draft/save:**

- Theo dõi form value change như draft của tab; PDFium cung cấp undo/redo cho
  editable field nhưng không cho choice field không editable.
- Export form snapshot bất đồng bộ hoặc blocking vào file tạm cho save pipeline.
- Reset/destroy worker session khi document/revision thay đổi hoặc controller bị
  hủy.

### [`src/PDFFormWorker.cpp`](../src/PDFFormWorker.cpp) và
[`src/PDFFormWorker.h`](../src/PDFFormWorker.h)

**Vai trò:** toàn bộ form session PDFium ở phía worker và hàng đợi render/form
tuần tự.

**Registry và ownership:**

- Registry theo `sessionId` giữ document path/revision, `FPDF_DOCUMENT`,
  `FPDF_FORMHANDLE`, form-fill callbacks, JavaScript platform và active page.
- Mở document riêng từ path; không nhận main document handle từ GUI.
- Tất cả public submission API lên một queue có serialization; transaction giữ
  process-wide PDFium lock ở outer boundary.

**Interaction:**

- Map Qt pointer/key/modifier sang PDFium form APIs, duy trì focus, caret,
  selection, cursor và invalidation state.
- Thực thi document/page/open/close/action hooks và thu internal go-to/external
  URI từ callback host bị sandbox.
- Page change đóng/mở form page đúng thứ tự; reset session hủy state cũ.

**Choice-field safety:**

- Inspect visible Widget annotation và chỉ trả custom `ChoiceField` cho fixed
  combo/list có geometry/options bounded, không editable, không commit-on-select
  và không có `/A`/`/AA` action nguy hiểm cho semantic thay thế.
- Tạo identity token từ field/widget state. Trước focus/mutate luôn resolve lại
  source page và annotation rồi so token; không tin annotation index cũ.
- Multi-select toggle giải quyết trên live worker state, không trên QML snapshot.

**Designer/save support:**

- Khám phá field do TrePDF tạo, trả id/name/tooltip/value/default/type/flags/
  appearance version/source page/normalized rect cho designer rehydrate.
- Export snapshot của form session để giá trị native/JavaScript calculation được
  đưa vào nguồn writer.
- `withPageLocked()` cho renderer dùng preview/form page cùng session dưới lock.

**Callback contract:** worker trả plain structs (`InteractionResult`,
`SnapshotResult`, `ChoiceFieldsResult`, ...); không giữ QObject GUI trong request.

### [`src/PDFFormView.cpp`](../src/PDFFormView.cpp) và
[`src/PDFFormView.h`](../src/PDFFormView.h)

**Type QML:** `PDFFormView`, kế thừa `QQuickItem` và là cầu input native.

- Nhận controller, form designer controller, form design mode, display page và
  interactive flag.
- Bật hover/mouse/focus/input method; chuyển tọa độ item sang normalized/device
  geometry mà form controller cần.
- Chuyển key press/release, shortcut clipboard, IME preedit/commit và focus out;
  cập nhật cursor Qt từ `cursorType` của controller.
- Expose `choiceFields` và invokable focus/select để `FormChoiceOverlay` dùng.
  Refresh bất đồng bộ khi document/page/generation/render state thay đổi và loại
  response stale.
- Khi form designer đang phủ một source-backed TrePDF field hoặc giữ tombstone
  xóa Text Field/Dropdown/List Box/Radio, chặn native pointer path tại vùng đó để một
  click không vừa tác động designer draft vừa sửa widget PDFium bên dưới.
- Khi inactive/pooled/controller đổi, clear focus, choice snapshot và pending
  refresh.

## Form designer và ghi AcroForm

### [`src/PDFFormDesignerController.cpp`](../src/PDFFormDesignerController.cpp)
và [`src/PDFFormDesignerController.h`](../src/PDFFormDesignerController.h)

**Vai trò:** `QAbstractListModel` cho field đang thiết kế và field TrePDF đã
rehydrate.

**Model:**

- Field có id, name, tooltip, value/default value, type, flags, giới hạn ký tự,
  display/source page, normalized rect, source-backed, appearance version và
  bitmask property đã thay đổi.
- Enum model có Text, Checkbox, RadioButton, Dropdown, Date, Signature, ListBox;
  UI và writer hoàn chỉnh hiện hỗ trợ Text/Checkbox/RadioButton/Dropdown/ListBox.
- Expose roles cho `FormFieldLayer` và các API `fieldData()`/`fieldsForPage()` cho
  QML.

**Thao tác:**

- Tạo field với tên duy nhất, default size/value/flags; helper riêng cho Text và
  Comb Field. Comb vẫn là type Text, mặc định năm ô.
- Đổi value/name/options/flags/max length/rect; `moveField()` chỉ đổi top-left,
  giữ width/height và clamp field vào trang. Text Field không multi-line được
  chuẩn hóa về một dòng; max length dương được áp ngay lên value. Flag Comb yêu
  cầu max length dương, loại trừ Multiline, và khi đặt max length về `0` thì
  controller tự gỡ Comb.
- Chọn field, quản lý text focus, activate source checkbox/radio, chọn giá trị
  Dropdown/List Box, remove field transient và tạo tombstone khi xóa Text Field,
  Dropdown, List Box hoặc Radio source-backed do TrePDF tạo. `name` của Radio là Group ID;
  đổi tên gộp/tách nhóm và chọn một member sẽ tắt các member khác cùng tên.
- Map field qua page insertion/deletion; source-backed field giữ source page
  identity để update hoặc remove đúng widget sau save.

**Rehydrate:**

- `prepareForDesignMode()` yêu cầu `PDFFormWorker` inspect TrePDF fields.
- Kiểm generation/document key trước khi thay model; đồng bộ appearance version
  để writer biết field nào chỉ cần normalize appearance.
- Dropdown/List Box giữ annotation index và identity token; đổi lựa chọn source-backed đi
  qua form worker rồi refresh model thay vì tạo một value draft tách khỏi PDFium.
- Text field có `/Ff` Comb hợp lệ cùng `/MaxLen` được map trở lại flag designer,
  nên cell count và appearance tiếp tục chỉnh được sau reopen.

**Save snapshot:**

- `saveFields()` trả field add/update/normalize hợp lệ và tombstone Text
  Field/Dropdown/List Box/Radio dưới dạng `Remove`; field còn lại kèm
  `changedProperties` để writer không ghi đè value native nếu người dùng không
  sửa value trong designer.
- Draft state của model tham gia guard chung qua edit controller/session.
- Discard/reset xóa tombstone rồi rehydrate field nguồn từ document.

### [`src/FormFieldLayer.cpp`](../src/FormFieldLayer.cpp) và
[`src/FormFieldLayer.h`](../src/FormFieldLayer.h)

**Type QML:** `FormFieldLayer`, `QQuickItem` có scene graph và pointer events.

**Render:**

- Lấy field model theo display page, visibility/source visibility và pooled state.
- Dựng scene graph nodes cho Text Field, Check Box, Radio Button, Dropdown, List Box và
  presentation của các type dự trù; vẽ border, background, text/check mark/radio
  dot/arrow và selected state. Với Text Field có flag Comb, layer chia đều rect
  theo max length và căn ký tự giữa từng ô.
- Dùng constants từ `PDFFormAppearance.h` để preview font, border và padding khớp
  appearance writer.
- Expose active field id/rect/value/options/type/flags/source-backed để QML đặt
  editor, frame và handles đúng vị trí.

**Pointer interaction:**

- Hit-test field từ trên xuống, chọn field và phát `fieldActivated`; checkbox và
  radio có đường activate qua form runtime khi source-backed.
- Khi `moveEnabled`, hover đặt `SizeAllCursor`; press giữ mouse grab, qua
  `startDragDistance` mới bắt đầu move để click thuần vẫn activate.
- Move preview chỉ sửa `RenderField` cục bộ; `finishFieldMove()` gọi
  `controller->moveField()` đúng một lần, còn cancel phục hồi model.
- Có invokable begin/update/finish/cancel để move handle và keyboard trong QML
  dùng cùng pipeline.
- Reset pointer/move khi controller, page, pooling, mode, visibility hoặc
  interactivity đổi.

**Ranh giới scene graph:** chỉ tạo/sửa node trong `updatePaintNode()`; QObject/
model state được snapshot thành dữ liệu render ở GUI thread.

### [`src/PDFAcroFormWriter.cpp`](../src/PDFAcroFormWriter.cpp) và
[`src/PDFAcroFormWriter.h`](../src/PDFAcroFormWriter.h)

**Vai trò:** coordinator cấp cao ghi các field designer vào một PDF intermediate.

**Input:** source file path, target path và vector `Field` chứa widget id/name/
tooltip/value/default/options/rect/page/type/flags/operation.

**Pipeline:**

1. Yêu cầu caller đang giữ PDFium process lock và mở intermediate bằng PDFium.
2. Với update không đổi value/options, đọc live data từ document để tránh
   designer snapshot cũ ghi đè thay đổi form runtime/JavaScript.
3. Chuẩn hóa Checkbox/Radio Button về `Yes`/`Off`; kiểm Dropdown/List Box
   options/selection và chuyển Required/ReadOnly/Radio/Combo/Comb thành form
   flags. Text Comb còn ghi `DoNotScroll`, yêu cầu `/MaxLen` dương và không cho
   đồng thời Multiline. Radio không đặt `NoToggleToOff` để PDF reader cho phép
   bỏ chọn. Với Remove, chỉ chấp nhận Text Field, Dropdown, List Box hoặc Radio
   source-backed có widget id hợp lệ.
4. Với field Add, tạo Square annotation placeholder có rect, `NM=widgetId` và
   Print flag. PDFium public API chưa tạo đầy đủ Widget dictionary cần thiết.
5. Save bản có placeholder qua `QSaveFile` và PDFium.
6. Gọi `PDFAcroFormCanonicalizer::finalizeFields()` để biến placeholder/update
   thành field/widget chuẩn, tạo appearance hoặc gỡ field/Widget cần remove.
   Mỗi Group ID Radio Button được ghi đúng một field cha trong
   `/AcroForm/Fields`; các Widget con có `/Parent` và export value riêng để lựa
   chọn loại trừ hoạt động ổn định trên các PDF reader bên ngoài.
7. Mở lại file bằng PDFium, duyệt widget và kiểm field add/update khớp đúng một
   lần, còn field Remove không tồn tại.

**Khả năng hiện tại:** Text, Checkbox, grouped Radio Button, fixed
single-select Dropdown và multi-select List Box hỗ trợ Add/Update; Text, Dropdown, List Box và Radio còn hỗ trợ
NormalizeAppearance. Remove hỗ trợ top-level, single-widget Text Field/Dropdown/List Box
và member Radio thuộc group do TrePDF tạo; group Radio còn lại được dựng lại
khi xóa. Date/Signature, Remove Checkbox và cấu trúc AcroForm ngoài phạm vi
TrePDF phải fail rõ ràng.

**Failure:** nếu canonicalize hoặc validation thất bại, xóa target intermediate
để caller không dùng một PDF form nửa hoàn chỉnh.

### [`src/PDFAcroFormCanonicalizer.cpp`](../src/PDFAcroFormCanonicalizer.cpp)
và [`src/PDFAcroFormCanonicalizer.h`](../src/PDFAcroFormCanonicalizer.h)

**Vai trò:** bộ đọc/ghi PDF mục đích hẹp để hoàn thiện AcroForm, liên kết reply,
appearance FreeText và normal appearance vector của text/drawing/Ink markup mà
PDFium public API không dựng đủ an toàn.

**Đọc cấu trúc:**

- Memory-map file, parse trailer/xref/object reference/dictionary/string đủ cho
  trường hợp AcroForm cần xử lý.
- Tìm Catalog, `/AcroForm`, `/Fields`, page annotation arrays, placeholder có
  `NM` khớp widget id và field/widget hiện có cần update/remove.
- Với reply, tìm child qua `/NM`, tìm parent qua thread id ổn định, promote
  direct annotation khi cần rồi thêm reference `/IRT` và name `/RT /R` bằng
  incremental update.
- Thu field names để phát hiện trùng tên và bảo toàn reference hiện hữu.
- Đây không phải PDF editor tổng quát; parser chỉ nên mở rộng khi writer có test
  fixture tương ứng.

**Sinh object:**

- Tạo/cập nhật Text Field Widget dictionary, field flags, `/MaxLen`, `/DA`,
  `/DR`, value và default value. Comb Field dùng `/FT /Tx`, cờ Comb chuẩn PDF và
  một cell cho mỗi ký tự theo `/MaxLen`.
- Render appearance text field thành image XObject có giới hạn tổng pixel, rồi
  tạo appearance stream để PDF reader hiển thị ổn định kể cả trước khi focus;
  field multi-line được wrap và căn trên trong appearance; field comb vẽ divider
  và căn từng ký tự giữa cell.
- Render FreeText thành appearance image nền trắng, viền đen và text Unicode
  wrap trong rectangle annotation; `/Contents` vẫn giữ nội dung semantic để
  reader khác có thể tìm/đọc comment.
- Với Highlight/Underline/Squiggly/Strikeout mới, tìm annotation qua `/NM`, đọc
  `/QuadPoints`, `/C`, `/CA` rồi tạo `/AP /N` Form XObject vector. Highlight
  fill với `/BM /Multiply`; ba subtype nét dùng stroke với `/BM /Normal`.
  Annotation nguồn không nằm trong request nên giữ nguyên appearance cũ.
- Với Square/Circle/Line/Polygon/PolyLine mới, tìm placeholder duy nhất qua
  `/NM`, ghi subtype/Rect/`/L`/`/Vertices` cùng `/C`, `/IC`, `/CA`, `/BS`, `/F`,
  `/IT`, `/Name`, `/LE` và `/AP` Form XObject vector. Effect/ending chưa có
  primitive appearance tương ứng bị từ chối thay vì lưu dictionary lệch hình.
- Với Ink mới, tìm cùng placeholder qua `/NM`, giữ từng stroke trong
  `/InkList`, ghi `/Subtype /Ink`, `/Rect`, `/C`, `/CA`, `/BS`, `/F` và `/AP`
  Form XObject. Stream dùng một path mở cho mỗi stroke với PDF round cap/join;
  dictionary và appearance được append atomically trong file staging.
- Tạo Checkbox Widget với `/AP` Off/Yes riêng, `/AS`, value/default value và
  vector check appearance.
- Gom Radio theo Group ID thành một field cha `/FT /Btn` có `/Kids`; mỗi Widget
  con giữ `/Parent`, export value riêng theo widget id và appearance Off/On.
- Tạo Dropdown Widget `/FT /Ch` với Combo flag, `/Opt`, `/V`, `/DV`, `/I`, `/DA`
  và appearance có nút mũi tên.
- Tạo List Box Widget `/FT /Ch` có MultiSelect nhưng không có Combo/Edit, với
  `/Opt`, mảng `/V`, toàn bộ chỉ số `/I`, `/DV`, `/TI`, `/DA` và appearance
  nhiều hàng làm nổi tất cả lựa chọn hiện tại.
- Chỉ cập nhật Dropdown có `/Opt` là mảng string trực tiếp; export/display pairs
  bị từ chối để không làm mất export value khi canonicalize.
- Với Remove Dropdown/List Box, chỉ nhận TrePDF field top-level có đúng một Widget trên
  page yêu cầu. Với Remove Radio, yêu cầu snapshot đủ group nguồn và dựng lại
  field cha từ các member còn lại. Cả hai cập nhật page `/Annots` và
  `/AcroForm /Fields` để bỏ reference tương ứng.
- Với normalize-only text field, giữ field data hiện hữu và chỉ thay appearance
  dictionary/version cần thiết.
- Bổ sung AcroForm vào Catalog nếu chưa có, cập nhật `/Fields`, default font
  resource và appearance version của TrePDF.

**Ghi:** không rewrite toàn file lần nữa; append incremental objects, xref và
trailer có `/Prev` trỏ xref cũ. Mọi giới hạn, reference mismatch hoặc parse lỗi
phải dừng trước khi append.

### [`src/PDFWidgetAnnotationSuppression.cpp`](../src/PDFWidgetAnnotationSuppression.cpp)
và [`src/PDFWidgetAnnotationSuppression.h`](../src/PDFWidgetAnnotationSuppression.h)

**Vai trò:** tạm ẩn Widget annotation cụ thể trong lúc render trang có form
designer, rồi phục hồi document và pixel nền an toàn.

- `suppress(page, widgetIds)` tìm Widget theo `/NM`, lưu annotation index/flags/
  page rect và thêm Hidden flag. Nếu thiếu bất kỳ id hoặc set flag lỗi, phục hồi
  ngay và báo false.
- Guard RAII gọi `restore()` trong destructor; caller phải giữ page mở và
  PDFium lock trong toàn bộ lifetime.
- `pageRects()` trả vùng cần patch.
- `capturePDFWidgetRenderPatches()` map bốn góc page rect sang device pixel và
  sao chép nền sau base render. Vùng được nới 4 px để bao cả dotted focus outline
  mà `FPDF_FFLDraw` có thể vẽ ngoài annotation rect.
- Sau khi restore Widget flags và gọi `FPDF_FFLDraw` cho toàn form,
  `restorePDFWidgetRenderPatches()` dán các vùng nền trở lại để field đang được
  designer vẽ không xuất hiện hai lần.
- Mọi patch kiểm format 32-bit, bounds và kích thước trước `memcpy`.

## Header hỗ trợ không có `.cpp`

### [`src/PDFFormAppearance.h`](../src/PDFFormAppearance.h)

- Chứa constants version cho appearance do TrePDF sinh, tách khỏi schema dữ liệu
  field để có thể migrate giao diện mà không mất validation properties.
- Chứa font size, border width/color và padding chuẩn của Text Field; cả
  `FormFieldLayer` và canonicalizer/writer phải dùng cùng giá trị.
- Text/checkbox/radio/dropdown/list-box có appearance version riêng; designer dùng version
  tương ứng để phát hiện field cũ cần normalize khi mở lại.

### [`src/PDFiumSynchronization.h`](../src/PDFiumSynchronization.h)

- Cấp một `QMutex` sống tới process exit vì destructor của thread-local PDFium
  document có thể chạy muộn trong lúc thread pool teardown.
- `PDFiumProcessLock` là RAII guard có Blocking/TryLock, thread-affine và cấm
  copy/move.
- Dùng thread-local flag để phát hiện lock lồng nhau và `qFatal` thay vì deadlock.
- `assertPDFiumProcessLockHeld()`/`NotHeld()` ghi rõ contract của helper.
- Không giữ lock khi emit signal, post callback hoặc chờ worker; không để helper
  con tự acquire lại lock do outer transaction đã giữ.

## Bài kiểm thử

### [`tests/tst_PDFEditController.cpp`](../tests/tst_PDFEditController.cpp)

**Vai trò:** bộ Qt Test tích hợp/hồi quy chính cho edit, save, form, font,
render concurrency và multi-tab. Tệp lớn vì tự dựng nhiều PDF fixture nhỏ để
kiểm round-trip bằng PDFium thay vì phụ thuộc tài liệu ngoài repository.

**Nhóm coverage hiện tại:**

- Form designer: lưu/rehydrate Text Field, Comb Field, Checkbox, grouped Radio
  Button, Dropdown và List Box multi-select năm lựa chọn mặc định; giữ toàn bộ
  lựa chọn/options khi move-resize; kiểm mutual
  exclusion, Group ID, max length/comb constraints, export/control index và xóa
  member Radio; xóa draft/Discard/Save round-trip cho TrePDF Dropdown/List Box;
  appearance normalization, AcroForm sẵn có và tên field trùng.
- Form runtime: JavaScript tự bật, choice field tùy biến và persistence, push
  button action, URI sandbox, external-link routing và PDFium serialization
  giữa thread.
- Candidate/hover: scan ngoài GUI thread, cancellation/reschedule/defer khi lock
  bận, grouping theo dòng/cột/style/font, giới hạn editable scope, comment-only
  selection/default tab tool và comment index/điều hướng xuyên trang.
- Search: tìm text không phân biệt hoa thường xuyên nhiều trang, normalized
  highlight/current result, Previous/Next wrap, thay query loại request cũ và
  document close/reset xóa state.
- Font: bundled/system registration, language filters, mixed-style intersection,
  embedding rejection, glyph coverage, TTC/subset collision và stretch
  calibration.
- Rich text: frame mở rộng, column boundary, resize/content bounds, selection
  formatting, typing format, paragraph alignment, editor undo và scale qua nhiều
  lần save.
- Content round-trip: text, Vietnamese, paint Fill/Stroke/pattern/grayscale,
  Highlight/Squiggly/Underline/Strikeout/Sticky Note/Free Text, image, shape,
  source move/delete, z-order, CropBox và rotated page. Drawing-comment coverage
  gồm move/resize source Square/Circle/Line/Polygon/PolyLine/Ink, transform
  endpoint/vertex/stroke, AP-preservation và các safety gate. Ink còn kiểm
  validate/dedup/simplify/cap, nhiều stroke rehydrate, bounded malformed-source
  fallback, contents/reply/delete/history, `/InkList` và vector `/AP` round
  cap/join.
- Page operation: insert/delete, undo/redo/discard, source page mapping và save.
- Save lifecycle: async save, failure giữ draft, Save/Save As và reopen.
- Multi-tab: document/session độc lập, rotation/Page Display/split/draft riêng,
  state zoom/scroll/page của từng pane không rò tab, save đúng tab và chặn Save
  As đè tab khác.
- Page Display: enum/mapping cover, model một delegate cho Single/Facing, điều
  hướng theo spread và hai kiểu Split với state pane tương ứng.

**Cách thêm regression:** tạo fixture nhỏ nhất tái hiện lỗi, kiểm cả state trước
save và PDF mở lại sau save. Với thay đổi renderer/scene graph, test này không
thay thế kiểm thử native Metal thủ công.

## Ánh xạ tệp triển khai và header

| Triển khai | Header/API công khai |
|---|---|
| `ApplicationFonts.cpp` | `ApplicationFonts.h` |
| `DraftImageItem.cpp` | `DraftImageItem.h` |
| `FormFieldLayer.cpp` | `FormFieldLayer.h` |
| `PDFAcroFormCanonicalizer.cpp` | `PDFAcroFormCanonicalizer.h` |
| `PDFAcroFormWriter.cpp` | `PDFAcroFormWriter.h` |
| `PDFAnnotationUtils.cpp` | `PDFAnnotationUtils.h` |
| `PDFDocument.cpp` | `PDFDocument.h` |
| `PDFEditController.cpp` | `PDFEditController.h` |
| `PDFFormController.cpp` | `PDFFormController.h` |
| `PDFFormDesignerController.cpp` | `PDFFormDesignerController.h` |
| `PDFFormView.cpp` | `PDFFormView.h` |
| `PDFFormWorker.cpp` | `PDFFormWorker.h` |
| `PDFPageItem.cpp` | `PDFPageItem.h` |
| `PDFSearchController.cpp` | `PDFSearchController.h` |
| `PDFSaveWriter.cpp` | `PDFSaveWriter.h` |
| `PDFTabModel.cpp` | `PDFTabModel.h` |
| `PDFWidgetAnnotationSuppression.cpp` | `PDFWidgetAnnotationSuppression.h` |
| `main.cpp` | Không có header riêng |

## Checklist khi sửa C++

- Xác định object/thread nào sở hữu dữ liệu và handle; không capture QObject thô
  vào worker nếu lifetime không được guard bằng `QPointer`.
- Với PDFium, đặt một lock duy nhất ở outer transaction và bảo đảm mọi handle/
  mutation tạm được restore/destroy trước khi unlock.
- Với async result, so toàn bộ identity/generation/revision chứ không chỉ page
  index.
- Với save, snapshot dữ liệu GUI-thread trước; worker không truy cập controller,
  QML, CoreText live hoặc đường dẫn ảnh ban đầu.
- Với source object, resolve tất cả path trước remove; ưu tiên matrix-only cho move
  để giữ resource và z-order.
- Với form designer, test add, update, normalize, rehydrate, remove Dropdown và
  validation sau mở lại; enum tồn tại không đồng nghĩa writer đã hỗ trợ type đó.
- Chạy build `trepdf`, `git diff --check` và test liên quan. Thay đổi render/lifecycle
  phải thử thêm trên Metal với scroll, zoom, close/reopen và quit.
