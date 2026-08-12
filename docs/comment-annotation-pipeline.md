# Kiến trúc và pipeline comment/annotation PDF

Tài liệu này mô tả implementation comment hiện tại của TrePDF, từ lúc người
dùng chọn công cụ trong tab Comment, quét annotation có sẵn, tạo draft,
undo/redo/discard, hiển thị trên trang, quản lý reply, đến lúc Save/Save As ghi
annotation chuẩn vào PDF và mở lại tệp đã lưu.

Phần **Implementation hiện tại** là hành vi đang có trong mã nguồn. Phần
**Gaps và roadmap** chỉ là định hướng; không được xem là tính năng đã hỗ trợ.

Các tệp chính:

- [`CommentRibbon.qml`](../src/Components/CommentRibbon.qml): command surface,
  tool, màu markup và history.
- [`CommentPanel.qml`](../src/Components/CommentPanel.qml): chỉ mục comment toàn
  tài liệu, sửa `/Contents`, reply một cấp, điều hướng và xóa.
- [`DocumentWorkspace.qml`](../src/Components/DocumentWorkspace.qml): mở panel
  Comment cho đúng document session.
- [`EditOverlay.qml`](../src/Components/EditOverlay.qml): gesture tạo annotation,
  preview, selection và draft presentation trên trang.
- [`SquigglyLine.qml`](../src/Components/SquigglyLine.qml): renderer zigzag dùng
  chung cho gesture và draft Squiggly.
- [`PDFEditController.h`](../src/PDFEditController.h) và
  [`PDFEditController.cpp`](../src/PDFEditController.cpp): candidate scan,
  comment index, selection, draft, reply, history và điều phối Save.
- [`PDFPageItem.cpp`](../src/PDFPageItem.cpp): render PDFium, ẩn annotation draft
  đã xóa và ẩn appearance độc lập của reply.
- [`PDFSaveWriter.h`](../src/PDFSaveWriter.h) và
  [`PDFSaveWriter.cpp`](../src/PDFSaveWriter.cpp): tạo, sửa và xóa annotation
  trên writer document riêng.
- [`PDFAcroFormCanonicalizer.h`](../src/PDFAcroFormCanonicalizer.h) và
  [`PDFAcroFormCanonicalizer.cpp`](../src/PDFAcroFormCanonicalizer.cpp): hoàn
  thiện `/IRT`, `/RT /R`, appearance Free Text và appearance vector cho bốn
  text-markup subtype trên file staging.
- [`tst_PDFEditController.cpp`](../tests/tst_PDFEditController.cpp): fixture và
  regression round-trip bằng PDFium.

## 1. Phạm vi hiện tại

TrePDF coi **comment** là tập con của markup annotation trong PDF. Widget của
AcroForm, Link, nội dung text của page và shape thuộc tab Edit không đi qua
pipeline này.

| Công cụ UI | `Candidate.kind` | PDF subtype | Geometry hiện tại |
|---|---|---|---|
| Highlight | `highlight` | `/Highlight` | Rectangle tự do, ghi thành một hoặc nhiều `/QuadPoints`. |
| Underline | `underline` | `/Underline` | Rectangle tự do, ghi thành `/QuadPoints`. |
| Squiggly | `squiggly` | `/Squiggly` | Giao gesture với glyph nguồn, snap thành rectangle/QuadPoints theo dòng. |
| Strikeout | `strikeout` | `/StrikeOut` | Rectangle tự do, ghi thành `/QuadPoints`. |
| Sticky Note | `stickyNote` | `/Text` | Frame cố định 24 PDF point tại vị trí click. |
| Free Text | `freeText` | `/FreeText` | Click dùng frame mặc định; drag dùng text box đã clamp trong page. |

Với cả sáu loại trên, implementation hiện tại hỗ trợ:

- tạo annotation mới dưới dạng session draft;
- đọc và index annotation đã có trong PDF;
- chọn và điều hướng từ CommentPanel;
- sửa `/Contents` trong CommentPanel;
- xóa, undo, redo và discard;
- thêm reply vào comment gốc;
- Save/Save As rồi rehydrate sau khi mở lại;
- hiển thị author và ngày sửa, fallback về ngày tạo;
- bảo vệ annotation ReadOnly hoặc LockedContents.

Giới hạn hiện tại:

- geometry của annotation nguồn không move/resize được;
- reply-to-reply không được tạo từ UI;
- reply chain nhập từ phần mềm khác được flatten thành một cấp hiển thị;
- mỗi comment hoặc reply tối đa 700 UTF-16 code unit;
- Rectangle, Ellipse, Line, Arrow, Triangle và Diamond trong tab Edit là page
  content path object, không phải drawing comment annotation;
- `/Ink`, `/Stamp`, `/Caret`, `/Line`, `/Square`, `/Circle`, `/Polygon`,
  `/PolyLine`, `/FileAttachment`, review state và XFDF chưa thuộc comment model.

## 2. Luồng kiến trúc tổng thể

```mermaid
flowchart LR
    Ribbon["CommentRibbon\ntool + color + commands"] --> Overlay["EditOverlay\ngesture + preview"]
    Ribbon --> Panel["CommentPanel\nindex + text + replies"]
    Overlay --> Controller["PDFEditController\ncandidate + draft + history"]
    Panel --> Controller
    Controller --> Scan["Annotation-only scan\nworker document + PDFium lock"]
    Controller --> PageItem["PDFPageItem\nrender + suppression"]
    Controller --> Prepare["PDFSaveWriter::prepare\nGUI-thread snapshot"]
    Prepare --> Writer["PDFSaveWriter::save\nworker writer document"]
    Writer --> Canon["PDFAcroFormCanonicalizer\nText-markup/FreeText AP + IRT/RT"]
    Canon --> Reopen["QSaveFile commit\nPDFDocument::open"]
```

Các ownership quan trọng:

- QML, `PDFDocument` và `PDFEditController` sống trên GUI thread.
- Candidate/comment scan mở document riêng từ file path dưới process-wide
  PDFium lock. GUI/QML không giữ `FPDF_PAGE` hoặc `FPDF_ANNOTATION`.
- Writer nhận snapshot bất biến, mở writer document riêng và không giữ con trỏ
  tới controller/document.
- Chỉ sau khi commit và reopen thành công, draft và history mới bị reset.

## 3. Mô hình dữ liệu

### 3.1 `Candidate`

Một comment nguồn hoặc comment mới được biểu diễn bằng `Candidate`. Các trường
quan trọng cho annotation gồm:

```text
page
objectId
sourceAnnotationIndex
sourceAnnotationName       // /NM
kind                       // highlight, stickyNote, freeText...
text                       // /Contents
geometry.sourceRect
highlightRects             // QuadPoints đã chuẩn hóa theo page
highlightColor
commentAuthor              // /T
commentCreationDate        // /CreationDate
commentModifiedDate        // /M
commentThreadId
replyToObjectId
replyToThreadId
replyToSourceAnnotationIndex
replyDirectParentSourceAnnotationIndex
commentTextReadOnly
added / addedOrder
```

`sourceAnnotationIndex` chỉ có ý nghĩa trong source page và document revision
đã quét. Writer không tin index một cách mù quáng: trước khi mutation, nó kiểm
lại subtype, `/Contents`, `/NM` nếu có và các cờ bảo vệ.

`objectId` của annotation nguồn có dạng `annotation:<index>`. Vì cùng index có
thể xuất hiện trên nhiều page, mọi API và draft key phải dùng cặp
`page + objectId`, không dùng `objectId` độc lập.

### 3.2 `Draft` và history

`Draft` giữ snapshot của candidate cùng text và geometry hiện hành:

```text
candidate
text
characterSpans
paragraphFormats
frameRect
contentBounds
deleted
```

Comment dùng chung `Command` snapshot với text/image/shape. Một action logic tạo
một command before/after; undo và redo phục hồi toàn bộ candidate/draft state.
Xóa annotation nguồn được biểu diễn bằng tombstone draft thay vì gỡ annotation
khỏi document đang mở.

`hasDrafts` còn tính cả pending Squiggly creation. Vì vậy thả chuột khi glyph
geometry chưa sẵn sàng vẫn kích hoạt draft guard; Discard phải hủy cả pending
request đó.

### 3.3 Tọa độ

Controller và draft lưu geometry theo normalized page coordinates `0..1` của
trang logic chưa xoay. `EditOverlay` đổi qua pixel bằng kích thước local của
`pageSurface`; writer đổi tiếp sang PDF page coordinates bằng
`FPDF_DeviceToPage()`.

View rotation chỉ là transform scene graph. Không đưa rotation của UI vào
candidate, draft hoặc save-time geometry.

## 4. Quét và lập chỉ mục comment

### 4.1 Candidate scan theo page

Scan page phục vụ hover/edit nội dung đồng thời đọc sáu annotation subtype được
hỗ trợ. Với mỗi annotation, controller lấy:

- `/Contents`, `/NM`, `/T`, `/CreationDate`, `/M`;
- `/Rect` và attachment points/QuadPoints của text markup;
- annotation color;
- flags ReadOnly và LockedContents;
- `/IRT` và `/RT` để nhận diện reply;
- private thread id của TrePDF nếu annotation đang ở giữa save pipeline.

Kết quả được cache theo display page và document revision. Khi revision đổi,
mọi kết quả cũ và callback đến muộn phải bị loại.

### 4.2 Annotation-only comment index

`requestCommentIndex()` chạy một lượt riêng qua mọi source page:

1. Mở source page trong worker document.
2. Chỉ gọi `appendAnnotationCandidates()`; không scan text/image page object.
3. Giữ lại comment candidate và nhả PDFium lock giữa các page.
4. Post kết quả về GUI thread nếu document/revision vẫn khớp.

Index riêng giúp CommentPanel liệt kê toàn tài liệu mà không làm cache
text/image của viewport tăng theo tổng số page. Request bị hủy khi document,
revision hoặc session thay đổi.

### 4.3 Sắp thứ tự hiển thị

Comment gốc được sắp theo page, vị trí trên-trái, insertion order rồi object id.
Replies của một root ưu tiên creation date, sau đó source annotation index và
geometry. Model trả về theo thứ tự `root, replies..., root, replies...`.

Quan hệ reply malformed, dangling hoặc cross-page không được tạo thành thread
sâu trong UI; item vẫn có thể được trình bày như comment top-level để người dùng
đọc được.

## 5. Tương tác QML

### 5.1 `CommentRibbon`

Khi mở tab Comment, ribbon chọn Highlight làm tool mặc định. Người dùng có thể
chuyển sang Sticky Note, Free Text, Squiggly, Underline hoặc Strikeout mà không
bật form designer.

Highlight và Squiggly dùng split button cùng color grid và system color picker.
Mỗi tool giữ màu riêng; Squiggly dùng màu đậm/opacity cao hơn vì nét mảnh.
History dùng chung `PDFEditController::undo()` và `redo()`.

### 5.2 `EditOverlay`

Overlay giữ gesture tạm trong QML và chỉ commit một draft command khi release:

- text markup nhận rectangle kéo;
- Sticky Note nhận một click;
- Free Text nhận click hoặc drag;
- preview được vẽ trên page surface trước khi texture mới sẵn sàng.

Free Text không dùng rich `TextEdit` của page-content text. Hộp được vẽ trên
trang, nhưng `/Contents` được sửa trong CommentPanel.

### 5.3 `CommentPanel`

Khi được load, panel yêu cầu annotation-only index. Mỗi card hiển thị subtype,
page, màu, author, ngày, nội dung và draft state. Chọn card sẽ:

1. commit editor comment đang mở nếu hợp lệ;
2. chuyển controller về Select;
3. chọn candidate bằng `page + objectId`;
4. phát `pageRequested()` để điều hướng.

Editor giữ buffer local để không tạo history command cho mỗi phím. Text được
commit khi mất focus, đổi item, thu gọn panel, đổi session hoặc trước Save.

QML cho phép text tạm vượt 700 UTF-16 code unit để không phá IME/local undo,
nhưng commit, gửi reply và Save bị chặn cho đến khi độ dài hợp lệ. C++ kiểm lại
cùng giới hạn; không dựa riêng vào validation QML.

## 6. Geometry text markup và Squiggly

Highlight, Underline và Strikeout hiện giữ free rectangle của gesture. Writer
đổi rectangle này thành QuadPoints và bounding `/Rect`.

Squiggly có pipeline riêng:

1. Candidate scan cache bbox normalized của glyph source-text không whitespace.
2. OCR Invisible được giữ dạng hit-test-only, không xuất hiện như text có thể
   select/edit.
3. QML gửi gesture rectangle normalized cho controller.
4. Controller giao gesture với glyph, nhóm theo visual line và snap thành các
   rectangle/QuadPoints theo dòng.
5. Vùng chỉ có whitespace hoặc không có glyph bị từ chối.

Nét gần nằm ngang dùng hit-test band tối thiểu ba pixel. Nếu glyph geometry vẫn
đang tải lúc release, controller giữ revision/page/selection/color, ưu tiên scan
page đó và tự materialize draft khi kết quả về.

Giới hạn an toàn:

- chỉ glyph ngang/upright được cache;
- glyph xoay/dọc cần oriented QuadPoints và hiện bị bỏ qua;
- page trên 50.000 text characters không giữ partial per-glyph cache;
- image-only page hoặc page không có usable text layer không hỗ trợ Squiggly.

`SquigglyLine` chỉ vẽ presentation. Hit-test, grouping và QuadPoints luôn thuộc
controller/writer, không được sao chép logic sang QML.

## 7. Reply và comment thread

### 7.1 Quan hệ chuẩn PDF

Reply mới được lưu thành `/Text` annotation. Sau khi canonicalize, dictionary có:

```text
/IRT <parent-reference>
/RT /R
```

TrePDF chỉ cho reply comment gốc. Reply không có action reply tiếp trong UI.
Khi nhập một PDF có chuỗi reply nhiều tầng, controller lần `/IRT` về root rồi
flatten các descendants thành một cấp hiển thị.

`/RT /Group` không phải reply relation và không đi qua thread logic hiện tại.
Review state dùng `/IRT` nhưng có `/State` và `/StateModel`; đây là gap cần phân
biệt rõ trước khi bổ sung status để state entry không bị coi là reply thường.

### 7.2 ID tạm trong save pipeline

PDFium public API không đủ để tạo mọi indirect `/IRT` relationship cần thiết.
Writer dùng các key private:

```text
/TrePDFThreadId
/TrePDFReplyTo
```

Các key này là metadata phụ để tìm parent/child ổn định trong file staging và
có thể vẫn tồn tại trong output. `PDFAcroFormCanonicalizer` tìm annotation qua
thread id và `/NM`, promote direct annotation thành indirect object nếu cần,
rồi thêm `/IRT` và `/RT /R` chuẩn. Khi mở lại, logic nghiệp vụ phải ưu tiên
relationship chuẩn; không được coi private key là sự thay thế cho `/IRT`.

### 7.3 Hiển thị reply

Reply tồn tại trong CommentPanel nhưng standalone annotation appearance của nó
không được vẽ trên page viewer. Trong render worker, `PDFPageItem` tạm ẩn reply
markup và Popup liên kết, render page, rồi phục hồi flags dưới PDFium lock.

Đây là presentation rule của TrePDF; reply semantic vẫn tồn tại và được ghi vào
PDF.

### 7.4 Xóa thread

- Xóa root tạo tombstone cho root và ẩn toàn thread trong session.
- Xóa reply chỉ tạo tombstone cho reply đó.
- Save lần `/IRT` descendants, xác thực markup subtype và xóa từ annotation
  index lớn xuống nhỏ.
- `/Popup` liên kết cũng bị xóa để không để lại orphan.
- Nếu chain chứa cấu trúc không thể xác thực an toàn, Save thất bại và giữ draft
  thay vì ghi file thiếu dữ liệu.

## 8. Permission và annotation flags

Mọi mutation comment phải vượt qua hai lớp kiểm tra:

1. document security cho phép sửa annotation;
2. source annotation không có ReadOnly hoặc LockedContents.

Annotation bị bảo vệ vẫn được index, hiển thị và điều hướng, nhưng không cho sửa
text hoặc xóa. Cả QML và controller đều có trạng thái disable/reason; writer vẫn
kiểm lại flags để chống source thay đổi giữa scan và Save.

Annotation nguồn hiện chỉ sửa `/Contents`; move/resize page appearance chưa được
hỗ trợ. Không được dùng draft geometry để âm thầm thay đổi source annotation
nếu UI chưa cung cấp mutation đó.

## 9. Render và draft presentation

Page texture sạch do PDFium render annotation nguồn. `EditOverlay` vẽ comment
mới hoặc Free Text đã đổi nội dung lên trên texture khi cần.

Với text markup đã có `/AP`, candidate scan coi appearance là nguồn màu hiển
thị: nếu `FPDFAnnot_GetColor()` từ chối annotation có AP, controller đọc màu
fill/stroke thực tế từ path object trong normal appearance. Không dùng `/C` hay
`/CA` stale để ghi đè màu/alpha đã đóng gói trong AP.

Các quy tắc tránh vẽ hai lần:

- source annotation bị draft xóa được tạm ẩn trong render worker;
- reply appearance được tạm ẩn nhưng reply vẫn ở CommentPanel;
- Free Text nguồn chưa đổi giữ appearance gốc;
- Free Text đã sửa được preview bằng hộp trắng, viền đen và text wrap của TrePDF;
- khi texture mới đã phản ánh đúng draft revision, overlay không chồng preview
  không cần thiết.

Render suppression phải luôn phục hồi annotation flags trước khi đóng page.
Page, document handle và process-wide PDFium lock phải sống trong toàn bộ
lifetime của guard.

## 10. Save và Save As

### 10.1 Chuẩn bị request

Trên GUI thread, controller:

1. commit editor comment đang mở;
2. từ chối Save nếu còn pending Squiggly geometry;
3. lấy form snapshot nếu form runtime có draft;
4. gọi `PDFSaveWriter::prepare()` để copy draft, page map, metadata và target;
5. đặt `saving = true` để khóa mutation/tab lifecycle.

Worker request không giữ controller, `PDFDocument`, QML object hoặc GUI-thread
PDFium handle.

### 10.2 Tạo annotation mới

Writer mở document riêng và tạo annotation bằng `FPDFPage_CreateAnnot()`:

- text markup: subtype tương ứng, color, Print flag, attachment points và Rect;
- Sticky Note: `/Text`, Rect, color, `/Contents`, metadata và Print flag;
- Free Text: `/FreeText`, Rect, `/Contents`, `/DA`, border, metadata, Print flag
  và appearance id cho canonicalizer;
- reply: tạo `/Text` placeholder rồi hoàn thiện `/IRT` và `/RT /R` ở bước
  canonicalize.

Sau khi PDFium serialize staging file, canonicalizer tìm text markup mới bằng
`/NM` và thêm `/AP << /N <form-ref> >>`. Normal appearance là Form XObject
vector có `/BBox`, identity `/Matrix` và `/ExtGState` chứa opacity. Highlight
dùng fill với blend mode `/Multiply`; Underline, Squiggly và Strikeout dùng
stroke với blend mode `/Normal`. Màu, alpha và geometry được đóng gói trong
appearance stream để reader không phải tự suy diễn từ `/C`, `/CA` và
`/QuadPoints`.

Metadata comment mới gồm `/NM`, `/T`, `/CreationDate` và `/M` khi đã sửa.
Author hiện lấy lần lượt từ `USER`, `LOGNAME`, tên home directory rồi application
name; chưa có user profile riêng trong UI.

### 10.3 Sửa annotation nguồn

Trước khi sửa, writer xác thực annotation index, subtype, original `/Contents`,
`/NM` nếu có và flags bảo vệ. Khi hợp lệ:

- cập nhật `/Contents` và `/M`;
- nếu annotation có `/RC`, thay bằng XHTML plain-text tương ứng để rich value
  không giữ nội dung cũ;
- giữ `/Popup`, `/IRT`, `/RT`, geometry, color và metadata không liên quan;
- với Free Text, xóa stale `/AP`, ghi appearance id và để canonicalizer tạo lại
  wrapped Unicode appearance.

Sticky Note và text markup giữ nguyên appearance nguồn vì sửa nội dung không
thay đổi page geometry. Canonicalizer chỉ nhận danh sách text markup mới, nên
không thay `/AP` của annotation nguồn. Free Text là ngoại lệ vì text hiển thị
trực tiếp trên trang.

### 10.4 Xóa annotation nguồn

Writer thu thập root/reply descendants và Popup trước mutation, xác thực subtype,
identity và relation, rồi xóa theo index giảm dần. Việc xóa comment chỉ thay
page annotation array; không được gọi `FPDFPage_GenerateContent()`.

### 10.5 Appearance và canonicalizer

Free Text được canonicalizer dựng `/AP` Unicode có wrap, nền trắng và viền đen;
`/Contents` vẫn là semantic text cho reader khác tìm kiếm và đọc.

Bốn text-markup subtype mới được dựng `/AP /N` vector từ chính `/QuadPoints`,
`/C` và `/CA` đã ghi trên annotation. `Rect`, `QuadPoints` và AP được tạo trong
cùng giao dịch staging; bất kỳ lỗi parse/canonicalize nào đều làm Save thất
bại trước khi target được commit. Source text markup giữ nguyên `/AP` hiện hữu.
Reply không có appearance độc lập.

Pipeline vẫn chưa phải generic appearance generator cho mọi annotation subtype.
Khi thêm drawing markup, Ink, Stamp hoặc khi nhắm PDF 2.0, mỗi subtype cần
chiến lược `/AP` riêng và regression mở bằng reader ngoài PDFium.

### 10.6 Transaction và reopen

- Nội dung trung gian được serialize bằng `FPDF_SaveAsCopy()` qua `QSaveFile`.
- Canonicalizer chỉ append incremental objects vào staging file.
- Chỉ commit target sau khi mọi bước thành công.
- Annotation-only page không regenerate content stream.
- Sau commit, `PDFDocument::open()` mở lại target và tăng revision.
- Draft/history chỉ reset sau reopen thành công.
- Nếu reopen thất bại sau commit, document stale phải đóng vì source annotation
  indices không còn đáng tin.

## 11. Invariant không được phá

1. GUI/QML không truy cập `FPDF_PAGE`, `FPDF_TEXTPAGE` hoặc
   `FPDF_ANNOTATION` của worker.
2. Không dùng main `FPDF_DOCUMENT` để Save.
3. Comment-only save không gọi `FPDFPage_GenerateContent()`.
4. Mọi async result phải khớp document, revision, page và request generation.
5. Source annotation mutation phải xác thực identity và flags ngay trước khi
   thay đổi.
6. Update `/Contents` không được làm mất Popup, thread relation, geometry hoặc
   metadata không thuộc mutation.
7. Xóa root phải xử lý descendants và Popup; không để orphan thread.
8. Reply appearance có thể bị ẩn trong viewer nhưng reply semantic không được
   bỏ khỏi index hoặc save request.
9. Normalized geometry không phụ thuộc view rotation.
10. Save failure phải giữ target cũ, draft và undo/redo để người dùng thử lại.
11. User-visible string mới phải dùng `qsTrId()`/`qtTrId()` literal với `//%` và
    cập nhật `i18n/qml_vi.ts`.

## 12. Kiểm thử

Regression chính nằm trong `tests/tst_PDFEditController.cpp`. Fixture nên là PDF
nhỏ nhất tự dựng được, sau đó kiểm cả state trước Save và file đã mở lại.

Coverage comment hiện cần được duy trì cho:

- round-trip Highlight, Underline, Squiggly và Strikeout;
- Squiggly glyph snap, OCR Invisible, pending scan, whitespace rejection,
  rotated glyph và safety cap;
- Sticky Note và Free Text Unicode appearance;
- author, creation/modification date và 700 UTF-16 code-unit limit;
- comment index xuyên trang, duplicate page-local `/NM` và navigation;
- sửa `/Contents`, `/M` và `/RC` của annotation nguồn;
- ReadOnly/LockedContents và document annotation permission;
- reply vào comment mới/nguồn, nested import flattening và standard `/IRT`;
- xóa reply, cascade root deletion, Popup và mixed-subtype descendants;
- undo/redo/discard trước Save;
- annotation-only Save không làm thay đổi page-content object count;
- CropBox, page rotation và Save/reopen lifecycle.

Lệnh kiểm tra chuẩn sau thay đổi:

```sh
/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  --build /Users/luongvanhuy/Documents/TreOffice/TrePDF/cmake-build-debug \
  --target trepdf -j 8

/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake \
  --build /Users/luongvanhuy/Documents/TreOffice/TrePDF/cmake-build-debug \
  --target trepdf_edit_controller_tests -j 8

ctest --test-dir \
  /Users/luongvanhuy/Documents/TreOffice/TrePDF/cmake-build-debug \
  --output-on-failure -R PDFEditControllerTests

git diff --check
```

Sau thay đổi appearance/render suppression, cần mở file kết quả bằng TrePDF và
ít nhất một reader khác trên macOS để kiểm tra interoperability thực tế.

## 13. Gaps và roadmap

Bảng này không mô tả tính năng đã có.

| Ưu tiên | Gap | Hướng triển khai |
|---|---|---|
| P0 | Common markup properties và AP cho subtype mới | Có model/preservation cho `/Subj`, `/F`, `/BS`, `/BE`, `/IC`, `/IT`, `/Name`; mở rộng AP theo từng drawing/Ink/Stamp subtype. |
| P0 | Read-only index cho mọi markup subtype | Hiển thị subtype chưa hỗ trợ trong panel mà không cho mutation cho đến khi writer an toàn. |
| P0 | Drawing comments chuẩn | Thêm `/Square`, `/Circle`, `/Line`, `/Polygon`, `/PolyLine`; giữ Shapes của tab Edit là page content riêng. |
| P0 | Pencil/freehand | Thêm `/Ink` với `/InkList`, point simplification, giới hạn điểm/stroke và AP. |
| P0 | Text-aware markup đồng nhất | Dùng glyph cache của Squiggly cho Highlight, Underline và Strikeout. |
| P1 | Sửa source geometry/style | Move/resize và property editor, giữ dictionary/AP/Popup/thread metadata. |
| P1 | Free Text nâng cao | `/IT /FreeTextCallout`, `/CL`, `/LE`, Typewriter, `/Q`, `/RC`/`DS`, border/fill/opacity. |
| P1 | Review state và grouping | Phân biệt `/State` + `/StateModel`, `/RT /R` và `/RT /Group`; hỗ trợ Accepted/Rejected/Completed/Marked. |
| P1 | Stamp và text-edit marks | `/Stamp`, `/Caret`, Insert/Replace review workflow. |
| P1 | Comment workflow | Search/filter/sort, author profile, export selected và import/export XFDF. |
| P2 | File attachment comment | `/FileAttachment` + `/FS`, byte snapshot, size limit và sandbox mở/extract. |
| P2 | Flattened copy/summary | Save a Flattened Copy riêng; cảnh báo mất semantic thread/metadata. |
| Riêng | Redaction | Mark `/Redact` và Apply Redaction phải là pipeline bảo mật riêng, không chỉ vẽ rectangle đen. |

Không ưu tiên cho Comment tab: Link/Widget, Movie, Sound, PrinterMark, TrapNet,
Watermark, 3D, Projection và RichMedia. Một số là annotation trong object model
PDF nhưng không phải comment workflow phổ thông; Movie, Sound và TrapNet còn là
legacy/deprecated trong PDF 2.0.

Reply nhiều cấp không phải ưu tiên: UI một cấp hiện tại vẫn giữ quan hệ root và
giúp thread dễ đọc. Nếu mở rộng, phải có fixture cho chain, cycle, cross-page và
orphan trước khi đổi model.

## 14. Checklist khi thêm subtype hoặc property comment

1. Thêm tool/kind và xác định rõ subtype là comment, page content hay form.
2. Mở rộng candidate scan và annotation-only index với giới hạn dữ liệu đầu vào.
3. Bổ sung `Candidate`/`Draft` fields và snapshot equality/history.
4. Thêm ribbon, cursor/gesture, overlay preview và CommentPanel label/property.
5. Xác định normalized geometry và phép đổi qua `FPDF_DeviceToPage()`.
6. Xác định `/Contents`, metadata, flags, subtype-specific dictionary và `/AP`.
7. Quyết định source annotation nào chỉ đọc, sửa được, move/resize được hoặc
   phải được bảo vệ.
8. Kiểm tra reply/state/group semantics; không coi mọi `/IRT` là reply thường.
9. Cập nhật render suppression để không vẽ source/draft/reply hai lần.
10. Cập nhật writer create/update/delete và canonicalizer nếu cần indirect refs.
11. Không regenerate page content nếu chỉ thay annotation.
12. Thêm translation ID, fixture, pre-save assertions và round-trip inspection.
13. Cập nhật tài liệu này, QML/C++ guide và source-file map nếu ownership đổi.

## 15. Tài liệu chuẩn tham khảo

- [PDF Association — ISO 32000-2:2020](https://pdfa.org/resource/iso-32000-2/)
- [PDF 2.0 errata — Clause 12, Interactive features](https://pdf-issues.pdfa.org/32000-2-2020/clause12.html)
- [Adobe PDF Reference 1.7 — Annotations](https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/pdfreference1.7old.pdf)
- [Adobe — Importing and exporting comments](https://helpx.adobe.com/acrobat/using/importing-exporting-comments.html)
